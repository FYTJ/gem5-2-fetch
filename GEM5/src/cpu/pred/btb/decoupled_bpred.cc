#include "cpu/pred/btb/decoupled_bpred.hh"

#include <algorithm>
#include <array>
#include <string>

#include "arch/riscv/regs/misc.hh"
#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/output.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/thread_context.hh"
#include "debug/BTB.hh"
#include "debug/DecoupleBPHist.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/Override.hh"
#include "debug/Profiling.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

namespace
{

void
writeJsonAddr(std::ostream &out, Addr addr)
{
    out << "\"0x" << std::hex << addr << std::dec << "\"";
}

std::string
branchKind(const BTBEntry &entry)
{
    if (entry.isReturn) {
        return "return";
    }
    if (entry.isCall) {
        return "call";
    }
    if (entry.isIndirect) {
        return "indirect";
    }
    if (entry.isCond) {
        return "cond";
    }
    return "uncond";
}

std::string
branchKind(const BranchInfo &branch)
{
    if (branch.isReturn) {
        return "return";
    }
    if (branch.isCall) {
        return "call";
    }
    if (branch.isIndirect) {
        return "indirect";
    }
    if (branch.isCond) {
        return "cond";
    }
    return "uncond";
}

std::string
branchSource(const BTBEntry &entry)
{
    if (entry.source < 0) {
        return "unknown";
    }
    return "component_" + std::to_string(entry.source);
}

const char *
twoFetchPredictionKindName(uint8_t kind)
{
    switch (kind) {
      case TWO_FETCH_PRED_SYNTHETIC_FALLBACK:
        return "synthetic_fallback";
      case TWO_FETCH_PRED_PREDICTOR_BACKED_CANDIDATE:
        return "predictor_backed_candidate";
      case TWO_FETCH_PRED_PREDICTOR_BACKED_ACCEPTED:
        return "predictor_backed_accepted";
      default:
        return "none";
    }
}

const char *
twoFetchPrefetchStateName(uint8_t state)
{
    switch (state) {
      case TWO_FETCH_PREFETCH_ISSUED:
        return "issued";
      case TWO_FETCH_PREFETCH_READY:
        return "ready";
      case TWO_FETCH_PREFETCH_USED:
        return "used";
      case TWO_FETCH_PREFETCH_DROPPED:
        return "dropped";
      case TWO_FETCH_PREFETCH_CANCELLED:
        return "cancelled";
      case TWO_FETCH_PREFETCH_STALE_RESPONSE:
        return "stale_response";
      default:
        return "not_issued";
    }
}

uint64_t
twoFetchStreamId(ThreadID tid, FetchTargetId fetch_id)
{
    return (static_cast<uint64_t>(tid) << 56) | fetch_id;
}

uint32_t
twoFetchSupplyRisk(const FetchTarget &parent, const FetchTarget &window1)
{
    uint32_t risk = TWO_FETCH_SUPPLY_RISK_NONE;
    if (parent.predTaken) {
        risk |= TWO_FETCH_SUPPLY_RISK_PARENT_TAKEN;
    }
    if (parent.overrideReason != OverrideReason::NO_OVERRIDE) {
        risk |= TWO_FETCH_SUPPLY_RISK_PARENT_OVERRIDE;
    }
    if (window1.predTaken) {
        risk |= TWO_FETCH_SUPPLY_RISK_WINDOW1_TAKEN;
    }
    if (window1.predTaken && window1.predBranchInfo.isReturn) {
        risk |= TWO_FETCH_SUPPLY_RISK_WINDOW1_RETURN;
    }
    if (window1.predTaken && window1.predBranchInfo.isIndirect) {
        risk |= TWO_FETCH_SUPPLY_RISK_WINDOW1_INDIRECT;
    }
    if (window1.predTaken && window1.predBranchInfo.isCall) {
        risk |= TWO_FETCH_SUPPLY_RISK_WINDOW1_CALL;
    }
    if (window1.predEndPC > window1.startPC &&
        ((window1.startPC >> 6) != ((window1.predEndPC - 1) >> 6))) {
        risk |= TWO_FETCH_SUPPLY_RISK_CROSS_CACHELINE;
    }
    if (window1.predEndPC > window1.startPC &&
        ((window1.startPC >> 12) != ((window1.predEndPC - 1) >> 12))) {
        risk |= TWO_FETCH_SUPPLY_RISK_CROSS_PAGE;
    }
    if (window1.predEndPC > window1.startPC &&
        (window1.predEndPC - window1.startPC) > 48) {
        risk |= TWO_FETCH_SUPPLY_RISK_LONG_WINDOW;
    }
    return risk;
}

} // anonymous namespace

uint8_t
DecoupledBPUWithBTB::getThreadAsidHash(ThreadID tid) const
{
    if (!cpu) {
        return 0;
    }

    const RegVal satp =
        cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_SATP, tid);
    const uint16_t asid = (satp >> 44) & mask(16);
    return foldAsidHash16To4(asid);
}

void
DecoupledBPUWithBTB::consumeFetchTarget(unsigned fetched_inst_num, ThreadID tid)
{
    FetchTargetId fetch_id = ftq.fetchId(tid);
    auto &target = ftq.fetching(tid);
    target.fetchInstNum = fetched_inst_num;
    if (target.twoFetchMaterialized) {
        target.twoFetchConsumed = true;
        target.twoFetchConsumedInsts = fetched_inst_num;
        target.twoFetchConsumeTick = curTick();
        if (target.twoFetchWindowSlot == 1) {
            dbpBtbStats.twoFetchWindow1Consumed++;
            dbpBtbStats.twoFetchWindow1ConsumedInsts += fetched_inst_num;
            dbpBtbStats.twoFetchWindow1StreamConsumed++;
        }
    }
    writeTwoAheadLifecycleTrace("fetch_consume", tid, fetch_id, target);
    ftq.finishTarget(tid);
}

DecoupledBPUWithBTB::DecoupledBPUWithBTB(const DecoupledBPUWithBTBParams &p)
    : BPredUnit(p),

      predictWidth(p.predictWidth),
      maxInstsNum(p.predictWidth / 2),
      historyBits(p.maxHistLen),
      ubtb(p.ubtb),
      abtb(p.abtb),
      mbtb(p.mbtb),
      microtage(p.microtage),
      tage(p.tage),
      ittage(p.ittage),
      mgsc(p.mgsc),
      ras(p.ras),
      // uras(p.uras),
      bpDBSwitches(p.bpDBSwitches),
      enableTwoAheadTwoTakenShadow(p.enableTwoAheadTwoTakenShadow),
      enableTwoAheadTwoTakenFunctional(p.enableTwoAheadTwoTakenFunctional),
      enableTwoFetch(p.enableTwoFetch),
      enableTwoFetchTrace(p.enableTwoFetchTrace),
      enableTwoFetchEnhancedGate(p.enableTwoFetchEnhancedGate),
      enableTwoFetchSyntheticFallback(p.enableTwoFetchSyntheticFallback),
      twoFetchOracleMode(p.twoFetchOracleMode),
      twoFetchGateMinFreeFTQ(p.twoFetchGateMinFreeFTQ),
      twoFetchUsefulnessThreshold(p.twoFetchUsefulnessThreshold),
      twoFetchUsefulnessMax(p.twoFetchUsefulnessMax),
      twoFetchUsefulnessExploreInterval(p.twoFetchUsefulnessExploreInterval),
      twoFetchUsefulnessExploreMinFreeFTQ(
          p.twoFetchUsefulnessExploreMinFreeFTQ),
      numStages(p.numStages),
      ftqEntries(p.ftq_size),
      ftqMode(p.smtFTQMode),
      ftqPolicy(p.smtFTQPolicy),
      smtFTQThreshold(p.smtFTQThreshold),
      ftq(p.numThreads, p.ftq_size),
      resolveBlockThreshold(p.resolveBlockThreshold),
      dbpBtbStats(this, p.numStages, p.fsq_size, maxInstsNum)
{
    panic_if(ftqMode == SMTFTQMode::Shared &&
             ftqPolicy == SMTFTQPolicy::Threshold &&
             smtFTQThreshold > ftqEntries,
             "SMT FTQ threshold (%u) exceeds total FTQ entries (%u)",
             smtFTQThreshold, ftqEntries);

    if (bpDBSwitches.size() > 0) {
        initDB();
    }
    if (enableTwoAheadTwoTakenShadow || enableTwoAheadTwoTakenFunctional ||
        (enableTwoFetch && enableTwoFetchTrace)) {
        twoAheadTraceStream =
            simout.create("bpu-two-ahead-two-taken.jsonl", false, true);
    }
    bpType = DecoupledBTBType;
    // Only add enabled components to the list
    if (ubtb->isEnabled()) components.push_back(ubtb);
    if (abtb->isEnabled()) components.push_back(abtb);
    if (microtage->isEnabled()) components.push_back(microtage);
    if (mbtb->isEnabled()) components.push_back(mbtb);
    if (tage->isEnabled()) components.push_back(tage);
    if (ras->isEnabled()) components.push_back(ras);
    if (ittage->isEnabled()) components.push_back(ittage);
    if (mgsc->isEnabled()) components.push_back(mgsc);
    numComponents = components.size();
    for (int i = 0; i < numComponents; i++) {
        components[i]->setComponentIdx(i);
        if (components[i]->hasDB) {
            bool enableDB = checkGivenSwitch(bpDBSwitches, components[i]->dbName);
            if (enableDB) {
                components[i]->enableDB = true;
                components[i]->setDB(&bpdb);
                components[i]->setTrace();
                removeGivenSwitch(bpDBSwitches, components[i]->dbName);
                someDBenabled = true;
            }
        }
    }
    if (bpDBSwitches.size() > 0) {
        warn("bpDBSwitches contains unknown switches\n");
        printf("unknown switches: ");
        for (auto it = bpDBSwitches.begin(); it != bpDBSwitches.end(); it++) {
            printf("%s ", it->c_str());
        }
        printf("\n");
    }

    historyManagers.reserve(numThreads);
    resolveDequeueFailCounters.assign(numThreads, 0);
    twoFetchUsefulnessTable.assign(
        std::max(1u, p.twoFetchUsefulnessTableSize), 0);
    twoFetchUsefulnessExploreCounters.assign(numThreads, 0);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        historyManagers.emplace_back(16);
    }

    for (int tid=0;tid<numThreads; tid++) {
        auto& thread = threads[tid];

        thread.s0PC = 0x80000000;
        thread.predsOfEachStage.resize(numStages);
        for (unsigned i = 0; i < numStages; i++) {
            thread.predsOfEachStage[i].predSource = i;
        }
        thread.s0History.resize(historyBits, 0);
        thread.s0PHistory.resize(historyBits, 0);
        thread.s0BwHistory.resize(historyBits, 0);
        thread.s0LHistory.resize(mgsc->getNumEntriesFirstLocalHistories());
        for (unsigned int i = 0; i < mgsc->getNumEntriesFirstLocalHistories(); ++i) {
            thread.s0LHistory[i].resize(historyBits, 0);
        }
        thread.commitHistory.resize(historyBits, 0);
        thread.squashing = true;
    }

    commitFsqEntryHasInstsVector.resize(maxInstsNum+1, 0);
    lastPhaseFsqEntryNumCommittedInstDist.resize(maxInstsNum+1, 0);
    commitFsqEntryFetchedInstsVector.resize(maxInstsNum+1, 0);
    lastPhaseFsqEntryNumFetchedInstDist.resize(maxInstsNum+1, 0);

    registerExitCallback([this]() {
        this->dumpStats();
    });
}

Addr
DecoupledBPUWithBTB::shadowFallthrough(Addr start) const
{
    return (start + predictWidth) & ~mask(floorLog2(predictWidth) - 1);
}

std::string
DecoupledBPUWithBTB::shadowMetadataId(ThreadID tid,
                                      FetchTargetId fetch_id,
                                      uint64_t epoch) const
{
    return "t" + std::to_string(tid) + "e" + std::to_string(epoch) +
        "m" + std::to_string(fetch_id);
}

std::vector<BTBEntry>
DecoupledBPUWithBTB::collectShadowTakenCandidates(FullBTBPrediction &pred)
{
    std::vector<BTBEntry> candidates;
    for (auto entry : pred.btbEntries) {
        if (!entry.valid) {
            continue;
        }

        bool taken = false;
        if (entry.isCond) {
            Addr branch_pc = entry.pc;
            auto it = CondTakens_find(pred.condTakens, branch_pc);
            taken = it != pred.condTakens.end() && it->second;
        } else if (entry.isUncond()) {
            taken = true;
        }

        if (!taken) {
            continue;
        }

        entry.target = pred.getEntryTarget(entry);
        candidates.push_back(entry);
        if (candidates.size() == 2 || entry.isUncond()) {
            break;
        }
    }
    return candidates;
}

void
DecoupledBPUWithBTB::writeTwoAheadPredictTrace(ThreadID tid,
                                               FetchTargetId fetch_id,
                                               FetchTarget &entry,
                                               FullBTBPrediction &pred,
                                               const FetchTarget *window1_entry,
                                               FetchTargetId window1_fetch_id)
{
    if ((!enableTwoAheadTwoTakenShadow && !enableTwoAheadTwoTakenFunctional &&
         !enableTwoFetch) ||
        !twoAheadTraceStream) {
        return;
    }

    auto &out = *twoAheadTraceStream->stream();
    const auto metadata_id = shadowMetadataId(tid, fetch_id, entry.shadowEpoch);
    const auto candidates = collectShadowTakenCandidates(pred);
    const Addr window0_pc = entry.startPC;
    const Addr window0_fallthrough = shadowFallthrough(window0_pc);
    const bool has_first_taken = !candidates.empty();
    const Addr window1_pc = has_first_taken ?
        candidates.front().target : window0_fallthrough;
    const Addr window1_fallthrough = shadowFallthrough(window1_pc);
    if (enableTwoAheadTwoTakenFunctional) {
        entry.twoAheadFunctionalCarried = true;
        entry.twoAheadWindowCount = 2;
        entry.twoAheadWindowValid[0] = true;
        entry.twoAheadWindowValid[1] = true;
        entry.twoAheadWindowPC[0] = window0_pc;
        entry.twoAheadWindowPC[1] = window1_pc;
        entry.twoAheadWindowFallthrough[0] = window0_fallthrough;
        entry.twoAheadWindowFallthrough[1] = window1_fallthrough;
        entry.twoAheadTakenCount = candidates.size();
        entry.twoAheadTakenValid.fill(false);
        entry.twoAheadTakenPC.fill(0);
        entry.twoAheadTakenTarget.fill(0);
        for (int slot = 0; slot < 2 && slot < candidates.size(); ++slot) {
            entry.twoAheadTakenValid[slot] = true;
            entry.twoAheadTakenPC[slot] = candidates[slot].pc;
            entry.twoAheadTakenTarget[slot] = candidates[slot].target;
        }
        dbpBtbStats.twoAheadFunctionalCarried++;
        dbpBtbStats.twoAheadFunctionalSecondWindowCarried++;
        if (candidates.size() == 2) {
            dbpBtbStats.twoAheadFunctionalTwoTakenCarried++;
        }
    }

    out << "{\"event\":\"predict_request\",\"cycle\":" << curTick()
        << ",\"pc\":";
    writeJsonAddr(out, window0_pc);
    out << ",\"fetch_id\":" << fetch_id
        << ",\"metadata_id\":\"" << metadata_id << "\""
        << ",\"epoch\":" << entry.shadowEpoch << "}\n";

    out << "{\"event\":\"predict_response\",\"cycle\":" << curTick()
        << ",\"pc\":";
    writeJsonAddr(out, window0_pc);
    out << ",\"fetch_id\":" << fetch_id
        << ",\"taken\":" << (entry.predTaken ? "true" : "false")
        << ",\"target\":";
    writeJsonAddr(out, entry.predTaken ? entry.getTakenTarget() : window0_fallthrough);
    const bool two_fetch_batch =
        enableTwoFetch && entry.twoFetchMaterialized &&
        entry.twoFetchBatchSize == 2 && window1_entry != nullptr;
    const bool two_fetch_rejected =
        enableTwoFetch && entry.twoFetchRejectReason != TWO_FETCH_REJECT_NONE;
    out << ",\"source\":\"shadow\""
        << ",\"metadata_id\":\"" << metadata_id << "\""
        << ",\"metadata_state\":\"allocated\""
        << ",\"epoch\":" << entry.shadowEpoch
        << ",\"mode\":\""
        << (two_fetch_batch ? "two_fetch" :
            (two_fetch_rejected ? "two_fetch_rejected" :
            (enableTwoAheadTwoTakenFunctional ? "functional" : "shadow"))) << "\""
        << ",\"functional_consumed_windows\":1"
        << ",\"functional_carried_second_window\":"
        << (entry.twoAheadFunctionalCarried ? "true" : "false")
        << ",\"functional_carry_evidence\":\""
        << (entry.twoAheadFunctionalCarried ? "fetch_target_metadata" : "none")
        << "\"";
    if (two_fetch_batch || two_fetch_rejected) {
        out << ",\"two_fetch_batch_id\":" << entry.twoFetchBatchId
            << ",\"two_fetch_batch_size\":"
            << unsigned(two_fetch_batch ? entry.twoFetchBatchSize : 1)
            << ",\"two_fetch_window_slot\":" << unsigned(entry.twoFetchWindowSlot)
            << ",\"two_fetch_materialized\":"
            << (two_fetch_batch ? "true" : "false")
            << ",\"two_fetch_prediction_kind\":\""
            << twoFetchPredictionKindName(entry.twoFetchPredictionKind) << "\""
            << ",\"two_fetch_predictor_backed\":"
            << (entry.twoFetchPredictorBacked ? "true" : "false")
            << ",\"two_fetch_prediction_accepted\":"
            << (entry.twoFetchPredictionAccepted ? "true" : "false")
            << ",\"two_fetch_pred_hit\":"
            << (entry.twoFetchPredHit ? "true" : "false")
            << ",\"two_fetch_pred_taken\":"
            << (entry.twoFetchPredTaken ? "true" : "false")
            << ",\"two_fetch_pred_return\":"
            << (entry.twoFetchPredReturn ? "true" : "false")
            << ",\"two_fetch_pred_indirect\":"
            << (entry.twoFetchPredIndirect ? "true" : "false")
            << ",\"two_fetch_reject_reason\":"
            << entry.twoFetchRejectReason
            << ",\"two_fetch_usefulness_index\":"
            << entry.twoFetchUsefulnessIndex;
    }
    out
        << ",\"ahead_windows\":[";

    out << "{\"slot\":0,\"valid\":true,\"pc\":";
    writeJsonAddr(out, window0_pc);
    out << ",\"fallthrough\":";
    writeJsonAddr(out, window0_fallthrough);
    out << ",\"source\":\"current\",\"epoch\":" << entry.shadowEpoch << "},";
    out << "{\"slot\":1,\"valid\":true,\"pc\":";
    writeJsonAddr(out, window1_pc);
    out << ",\"fallthrough\":";
    writeJsonAddr(out, window1_fallthrough);
    out << ",\"source\":\"" << (has_first_taken ? "target" : "fallthrough")
        << "\",\"epoch\":" << entry.shadowEpoch << "}]";

    out << ",\"taken_slots\":[";
    for (int slot = 0; slot < 2; ++slot) {
        if (slot > 0) {
            out << ",";
        }
        if (slot < candidates.size()) {
            const auto &candidate = candidates[slot];
            out << "{\"slot\":" << slot
                << ",\"valid\":true,\"window_slot\":0,\"pc\":";
            writeJsonAddr(out, candidate.pc);
            out << ",\"target\":";
            writeJsonAddr(out, candidate.target);
            out << ",\"source\":\"" << branchSource(candidate) << "\""
                << ",\"kind\":\"" << branchKind(candidate) << "\""
                << ",\"epoch\":" << entry.shadowEpoch << "}";
        } else {
            out << "{\"slot\":" << slot << ",\"valid\":false}";
        }
    }
    out << "]}\n";

    if (two_fetch_batch) {
        const auto window1_metadata_id =
            shadowMetadataId(tid, window1_fetch_id, window1_entry->shadowEpoch);
        const Addr w1_pc = window1_entry->startPC;
        const Addr w1_fallthrough = window1_entry->predEndPC;
        const Addr w1_next = shadowFallthrough(w1_fallthrough);

        out << "{\"event\":\"predict_response\",\"cycle\":" << curTick()
            << ",\"pc\":";
        writeJsonAddr(out, w1_pc);
        out << ",\"fetch_id\":" << window1_fetch_id
            << ",\"taken\":" << (window1_entry->predTaken ? "true" : "false")
            << ",\"target\":";
        writeJsonAddr(out, window1_entry->predTaken ?
            window1_entry->getTakenTarget() : w1_fallthrough);
        out << ",\"source\":\"two_fetch\""
            << ",\"metadata_id\":\"" << window1_metadata_id << "\""
            << ",\"metadata_state\":\"allocated\""
            << ",\"epoch\":" << window1_entry->shadowEpoch
            << ",\"mode\":\"two_fetch\""
            << ",\"two_fetch_batch_id\":" << window1_entry->twoFetchBatchId
            << ",\"two_fetch_batch_size\":"
            << unsigned(window1_entry->twoFetchBatchSize)
            << ",\"two_fetch_window_slot\":"
            << unsigned(window1_entry->twoFetchWindowSlot)
            << ",\"two_fetch_parent_fetch_id\":"
            << window1_entry->twoFetchParentFetchId
            << ",\"two_fetch_stream_id\":"
            << window1_entry->twoFetchStreamId
            << ",\"two_fetch_epoch\":"
            << window1_entry->twoFetchEpoch
            << ",\"two_fetch_prefetch_state\":\""
            << twoFetchPrefetchStateName(
                   window1_entry->twoFetchPrefetchState) << "\""
            << ",\"two_fetch_supply_risk\":"
            << window1_entry->twoFetchSupplyRisk
            << ",\"two_fetch_materialized\":true"
            << ",\"two_fetch_prediction_kind\":\""
            << twoFetchPredictionKindName(
                   window1_entry->twoFetchPredictionKind) << "\""
            << ",\"two_fetch_predictor_backed\":"
            << (window1_entry->twoFetchPredictorBacked ? "true" : "false")
            << ",\"two_fetch_prediction_accepted\":"
            << (window1_entry->twoFetchPredictionAccepted ? "true" : "false")
            << ",\"two_fetch_pred_hit\":"
            << (window1_entry->twoFetchPredHit ? "true" : "false")
            << ",\"two_fetch_pred_taken\":"
            << (window1_entry->twoFetchPredTaken ? "true" : "false")
            << ",\"two_fetch_pred_return\":"
            << (window1_entry->twoFetchPredReturn ? "true" : "false")
            << ",\"two_fetch_pred_indirect\":"
            << (window1_entry->twoFetchPredIndirect ? "true" : "false")
            << ",\"two_fetch_reject_reason\":"
            << window1_entry->twoFetchRejectReason
            << ",\"two_fetch_usefulness_index\":"
            << window1_entry->twoFetchUsefulnessIndex
            << ",\"ahead_windows\":[";
        out << "{\"slot\":0,\"valid\":true,\"pc\":";
        writeJsonAddr(out, w1_pc);
        out << ",\"fallthrough\":";
        writeJsonAddr(out, w1_fallthrough);
        out << ",\"source\":\"two_fetch\",\"epoch\":"
            << window1_entry->shadowEpoch << "},";
        out << "{\"slot\":1,\"valid\":true,\"pc\":";
        writeJsonAddr(out, w1_fallthrough);
        out << ",\"fallthrough\":";
        writeJsonAddr(out, w1_next);
        out << ",\"source\":\"fallthrough\",\"epoch\":"
            << window1_entry->shadowEpoch << "}]"
            << ",\"taken_slots\":[";
        if (window1_entry->predTaken) {
            out << "{\"slot\":0,\"valid\":true,\"window_slot\":0,\"pc\":";
            writeJsonAddr(out, window1_entry->predBranchInfo.pc);
            out << ",\"target\":";
            writeJsonAddr(out, window1_entry->getTakenTarget());
            out << ",\"source\":\"component_"
                << window1_entry->predSource << "\""
                << ",\"kind\":\""
                << branchKind(window1_entry->predBranchInfo) << "\""
                << ",\"epoch\":" << window1_entry->shadowEpoch << "},";
        } else {
            out << "{\"slot\":0,\"valid\":false},";
        }
        out << "{\"slot\":1,\"valid\":false}]}\n";
    }
    out.flush();
}

bool
DecoupledBPUWithBTB::isTwoFetchWindow1(const FetchTarget &target) const
{
    return target.twoFetchMaterialized && target.twoFetchWindowSlot == 1;
}

void
DecoupledBPUWithBTB::accountTwoFetchWindow1HistorySnapshot(
    const FetchTarget &target)
{
    if (!isTwoFetchWindow1(target)) {
        return;
    }

    dbpBtbStats.twoFetchHistorySnapshot++;
    if (ras->isEnabled()) {
        dbpBtbStats.twoFetchRasSnapshot++;
    }
}

void
DecoupledBPUWithBTB::accountTwoFetchWindow1HistoryRestore(
    const FetchTarget &target)
{
    if (!isTwoFetchWindow1(target)) {
        return;
    }

    dbpBtbStats.twoFetchHistoryRestore++;
    if (ras->isEnabled()) {
        dbpBtbStats.twoFetchRasRestore++;
    }
}

void
DecoupledBPUWithBTB::writeTwoAheadLifecycleTrace(const char *event,
                                                 ThreadID tid,
                                                 FetchTargetId fetch_id,
                                                 const FetchTarget &target)
{
    if ((!enableTwoAheadTwoTakenShadow && !enableTwoAheadTwoTakenFunctional &&
         !enableTwoFetch) ||
        !twoAheadTraceStream) {
        return;
    }

    auto &out = *twoAheadTraceStream->stream();
    const auto metadata_id = shadowMetadataId(tid, fetch_id, target.shadowEpoch);
    const std::string event_name(event);
    const std::string metadata_state =
        event_name == "resolve" ? "resolved" :
        event_name == "train" ? "trained" :
        event_name == "commit" ? "committed" : event_name;
    const bool fetch_consume = event_name == "fetch_consume" ||
        event_name == "fetch_window_consume";
    const Addr event_pc = fetch_consume ? target.startPC : target.exeTaken ?
        target.exeBranchInfo.pc : target.startPC;
    const Addr event_target = fetch_consume ? target.predEndPC : target.exeTaken ?
        target.exeBranchInfo.target : target.predEndPC;

    out << "{\"event\":\"" << event << "\",\"cycle\":" << curTick()
        << ",\"pc\":";
    writeJsonAddr(out, event_pc);
    if (fetch_consume) {
        out << ",\"fetch_id\":" << fetch_id
            << ",\"start_pc\":";
        writeJsonAddr(out, target.startPC);
        out << ",\"pred_end_pc\":";
        writeJsonAddr(out, target.predEndPC);
        out << ",\"consumed_insts\":"
            << (target.twoFetchMaterialized ?
                target.twoFetchConsumedInsts : target.fetchInstNum)
            << ",\"fetch_latency_ticks\":"
            << (target.twoFetchConsumeTick >= target.predTick ?
                target.twoFetchConsumeTick - target.predTick : 0);
    } else if (event_name == "resolve") {
        out << ",\"fetch_id\":" << fetch_id
            << ",\"actual_taken\":" << (target.exeTaken ? "true" : "false")
            << ",\"actual_target\":";
        writeJsonAddr(out, event_target);
    } else if (event_name == "train") {
        out << ",\"outcome\":{\"taken\":"
            << (target.exeTaken ? "true" : "false")
            << ",\"target\":";
        writeJsonAddr(out, event_target);
        out << "},\"tables\":[\"shadow\"]";
    } else if (event_name == "commit") {
        out << ",\"fetch_id\":" << fetch_id;
    }
    out << ",\"metadata_id\":\"" << metadata_id << "\""
        << ",\"metadata_state\":\"" << metadata_state << "\""
        << ",\"epoch\":" << target.shadowEpoch;
    if (target.twoFetchMaterialized) {
        out << ",\"mode\":\"two_fetch\""
            << ",\"two_fetch_batch_id\":" << target.twoFetchBatchId
            << ",\"two_fetch_batch_size\":"
            << unsigned(target.twoFetchBatchSize)
            << ",\"two_fetch_window_slot\":"
            << unsigned(target.twoFetchWindowSlot)
            << ",\"two_fetch_materialized\":true"
            << ",\"two_fetch_prediction_kind\":\""
            << twoFetchPredictionKindName(target.twoFetchPredictionKind) << "\""
            << ",\"two_fetch_predictor_backed\":"
            << (target.twoFetchPredictorBacked ? "true" : "false")
            << ",\"two_fetch_prediction_accepted\":"
            << (target.twoFetchPredictionAccepted ? "true" : "false")
            << ",\"two_fetch_parent_invalidated\":"
            << (target.twoFetchParentInvalidated ? "true" : "false")
            << ",\"two_fetch_reject_reason\":"
            << target.twoFetchRejectReason
            << ",\"two_fetch_usefulness_index\":"
            << target.twoFetchUsefulnessIndex;
        if (target.twoFetchWindowSlot == 1) {
            out << ",\"two_fetch_parent_fetch_id\":"
                << target.twoFetchParentFetchId
                << ",\"two_fetch_stream_id\":"
                << target.twoFetchStreamId
                << ",\"two_fetch_epoch\":"
                << target.twoFetchEpoch
                << ",\"two_fetch_prefetch_state\":\""
                << twoFetchPrefetchStateName(target.twoFetchPrefetchState)
                << "\""
                << ",\"two_fetch_supply_risk\":"
                << target.twoFetchSupplyRisk;
        }
    }
    out << "}\n";
    out.flush();
}

void
DecoupledBPUWithBTB::writeTwoFetchSupplyTrace(const char *event,
                                              ThreadID tid,
                                              FetchTargetId fetch_id,
                                              uint64_t stream_id,
                                              uint64_t epoch,
                                              Addr cacheline_base,
                                              unsigned line_index,
                                              const char *destination,
                                              const char *reason,
                                              bool stale,
                                              unsigned bytes)
{
    if (!enableTwoFetch || !twoAheadTraceStream) {
        return;
    }

    auto &out = *twoAheadTraceStream->stream();
    out << "{\"event\":\"" << event << "\",\"cycle\":" << curTick()
        << ",\"tid\":" << tid
        << ",\"fetch_id\":" << fetch_id
        << ",\"mode\":\"two_fetch\""
        << ",\"two_fetch_window_slot\":1"
        << ",\"two_fetch_stream_id\":" << stream_id
        << ",\"two_fetch_epoch\":" << epoch
        << ",\"cacheline_base\":";
    writeJsonAddr(out, cacheline_base);
    out << ",\"line_index\":" << line_index
        << ",\"request_type\":\"prefetch\"";
    if (destination) {
        out << ",\"destination\":\"" << destination << "\"";
    }
    if (reason) {
        out << ",\"reason\":\"" << reason << "\"";
    }
    if (bytes > 0) {
        out << ",\"bytes\":" << bytes;
    }
    out << ",\"stale\":" << (stale ? "true" : "false") << "}\n";
    out.flush();
}

bool
DecoupledBPUWithBTB::isLiveTwoFetchStream(ThreadID tid,
                                          FetchTargetId fetch_id,
                                          uint64_t stream_id,
                                          uint64_t epoch)
{
    if (!ftq.hasTarget(fetch_id, tid)) {
        return false;
    }

    const auto &target = ftq.get(fetch_id, tid);
    return isTwoFetchWindow1(target) &&
        target.twoFetchStreamId == stream_id &&
        target.twoFetchEpoch == epoch &&
        !target.twoFetchParentInvalidated &&
        !target.twoFetchConsumed &&
        target.twoFetchPrefetchState != TWO_FETCH_PREFETCH_CANCELLED &&
        target.twoFetchPrefetchState != TWO_FETCH_PREFETCH_DROPPED &&
        target.twoFetchPrefetchState != TWO_FETCH_PREFETCH_STALE_RESPONSE;
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchEligible(ThreadID tid,
                                                  FetchTargetId fetch_id,
                                                  uint64_t stream_id,
                                                  uint64_t epoch,
                                                  Addr cacheline_base)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchEligible++;
    writeTwoFetchSupplyTrace("two_fetch_prefetch_eligible", tid, fetch_id,
                             stream_id, epoch, cacheline_base, 0,
                             "fetch", "eligible", false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchQueued(ThreadID tid,
                                                FetchTargetId fetch_id,
                                                uint64_t stream_id,
                                                uint64_t epoch,
                                                Addr cacheline_base)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchQueued++;
    writeTwoFetchSupplyTrace("two_fetch_prefetch_queued", tid, fetch_id,
                             stream_id, epoch, cacheline_base, 0,
                             "queue", "queued", false);
}

void
DecoupledBPUWithBTB::noteTwoFetchSupplyQueueScan(
    ThreadID tid, FetchTargetId fetch_id, uint64_t stream_id,
    uint64_t epoch, Addr cacheline_base, unsigned ftq_distance)
{
    dbpBtbStats.twoFetchWindow1SupplyQueueScanned++;
    dbpBtbStats.twoFetchWindow1SupplyQueueDistanceSamples++;
    dbpBtbStats.twoFetchWindow1SupplyQueueDistanceTotal += ftq_distance;
    writeTwoFetchSupplyTrace("two_fetch_supply_queue_scan", tid, fetch_id,
                             stream_id, epoch, cacheline_base, 0,
                             "supply_queue", "scan", false);
}

void
DecoupledBPUWithBTB::noteTwoFetchSupplyQueueSelected(
    ThreadID tid, FetchTargetId fetch_id, uint64_t stream_id,
    uint64_t epoch, Addr cacheline_base, unsigned ftq_distance,
    unsigned line_index)
{
    (void)ftq_distance;
    dbpBtbStats.twoFetchWindow1SupplyQueueSelected++;
    writeTwoFetchSupplyTrace("two_fetch_supply_queue_select", tid, fetch_id,
                             stream_id, epoch, cacheline_base, line_index,
                             "supply_queue", "selected", false);
}

void
DecoupledBPUWithBTB::noteTwoFetchSupplyQueueEmpty(ThreadID tid)
{
    dbpBtbStats.twoFetchWindow1SupplyQueueEmpty++;
    writeTwoFetchSupplyTrace("two_fetch_supply_queue_empty", tid, 0, 0, 0, 0,
                             0, "supply_queue", "empty", false);
}

void
DecoupledBPUWithBTB::noteTwoFetchSupplyQueueBlocked(
    ThreadID tid, FetchTargetId fetch_id, uint64_t stream_id,
    uint64_t epoch, Addr cacheline_base, const char *reason)
{
    const std::string why(reason ? reason : "unknown");
    if (why == "outstanding_full") {
        dbpBtbStats.twoFetchWindow1SupplyQueueBlockedOutstanding++;
    } else if (why == "cache_blocked") {
        dbpBtbStats.twoFetchWindow1SupplyQueueBlockedCacheBlocked++;
    } else if (why == "retry_busy") {
        dbpBtbStats.twoFetchWindow1SupplyQueueBlockedRetry++;
    } else if (why == "too_close") {
        dbpBtbStats.twoFetchWindow1SupplyQueueBlockedTooClose++;
    } else {
        dbpBtbStats.twoFetchWindow1SupplyQueueBlockedDemandBusy++;
    }
    writeTwoFetchSupplyTrace("two_fetch_supply_queue_blocked", tid, fetch_id,
                             stream_id, epoch, cacheline_base, 0,
                             "supply_queue", why.c_str(), false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchBlocked(ThreadID tid,
                                                 FetchTargetId fetch_id,
                                                 uint64_t stream_id,
                                                 uint64_t epoch,
                                                 Addr cacheline_base,
                                                 const char *reason)
{
    const std::string why(reason ? reason : "unknown");
    if (why == "recovery_risk") {
        dbpBtbStats.twoFetchWindow1IcachePrefetchBlockedRecoveryRisk++;
    } else if (why == "prefetch_state") {
        dbpBtbStats.twoFetchWindow1IcachePrefetchBlockedState++;
    } else if (why == "no_window_bytes") {
        dbpBtbStats.twoFetchWindow1IcachePrefetchBlockedNoBytes++;
    } else {
        dbpBtbStats.twoFetchWindow1IcachePrefetchBlockedDemandBusy++;
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_blocked", tid, fetch_id,
                             stream_id, epoch, cacheline_base, 0,
                             "blocked", why.c_str(), false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchRequest(ThreadID tid,
                                                 FetchTargetId fetch_id,
                                                 uint64_t stream_id,
                                                 uint64_t epoch,
                                                 Addr cacheline_base,
                                                 unsigned line_index)
{
    if (ftq.hasTarget(fetch_id, tid)) {
        auto &target = ftq.get(fetch_id, tid);
        if (isTwoFetchWindow1(target) &&
            target.twoFetchStreamId == stream_id &&
            target.twoFetchEpoch == epoch &&
            target.twoFetchPrefetchState == TWO_FETCH_PREFETCH_NOT_ISSUED) {
            target.twoFetchPrefetchState = TWO_FETCH_PREFETCH_ISSUED;
        }
    }
    dbpBtbStats.twoFetchWindow1IcachePrefetchReq++;
    if (line_index == 0) {
        dbpBtbStats.twoFetchWindow1IcachePrefetchIssueLine0++;
    } else if (line_index == 1) {
        dbpBtbStats.twoFetchWindow1IcachePrefetchIssueLine1++;
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_request", tid, fetch_id,
                             stream_id, epoch, cacheline_base, line_index,
                             nullptr, nullptr, false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchMaterializeToRequest(Tick ticks)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchMaterializeToReqSamples++;
    dbpBtbStats.twoFetchWindow1IcachePrefetchMaterializeToReqTicks += ticks;
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchResponse(ThreadID tid,
                                                  FetchTargetId fetch_id,
                                                  uint64_t stream_id,
                                                  uint64_t epoch,
                                                  Addr cacheline_base,
                                                  unsigned line_index,
                                                  const char *destination,
                                                  bool stale)
{
    if (stale) {
        dbpBtbStats.twoFetchWindow1IcachePrefetchStaleResponse++;
        if (ftq.hasTarget(fetch_id, tid)) {
            auto &target = ftq.get(fetch_id, tid);
            if (isTwoFetchWindow1(target) &&
                target.twoFetchStreamId == stream_id &&
                target.twoFetchEpoch == epoch &&
                target.twoFetchPrefetchState != TWO_FETCH_PREFETCH_USED) {
                target.twoFetchPrefetchState =
                    TWO_FETCH_PREFETCH_STALE_RESPONSE;
            }
        }
    } else {
        dbpBtbStats.twoFetchWindow1IcachePrefetchReady++;
        if (ftq.hasTarget(fetch_id, tid)) {
            auto &target = ftq.get(fetch_id, tid);
            if (isTwoFetchWindow1(target) &&
                target.twoFetchStreamId == stream_id &&
                target.twoFetchEpoch == epoch &&
                target.twoFetchPrefetchState != TWO_FETCH_PREFETCH_USED) {
                target.twoFetchPrefetchState = TWO_FETCH_PREFETCH_READY;
            }
        }
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_response", tid, fetch_id,
                             stream_id, epoch, cacheline_base, line_index,
                             destination, nullptr, stale);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchLateResponse(ThreadID tid,
                                                      FetchTargetId fetch_id,
                                                      uint64_t stream_id,
                                                      uint64_t epoch,
                                                      Addr cacheline_base,
                                                      unsigned line_index)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchLateResponse++;
    writeTwoFetchSupplyTrace("two_fetch_prefetch_response", tid, fetch_id,
                             stream_id, epoch, cacheline_base, line_index,
                             "prefetch_buffer", "late_after_demand", false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchRequestToReady(Tick ticks)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchReqToReadySamples++;
    dbpBtbStats.twoFetchWindow1IcachePrefetchReqToReadyTicks += ticks;
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchDrop(ThreadID tid,
                                              FetchTargetId fetch_id,
                                              uint64_t stream_id,
                                              uint64_t epoch,
                                              Addr cacheline_base,
                                              unsigned line_index,
                                              const char *reason,
                                              bool stale)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchDropped++;
    if (stale) {
        dbpBtbStats.twoFetchWindow1IcachePrefetchStaleResponse++;
    }
    const bool drop_invalidates_stream = stale || line_index == 0;
    if (drop_invalidates_stream && ftq.hasTarget(fetch_id, tid)) {
        auto &target = ftq.get(fetch_id, tid);
        if (isTwoFetchWindow1(target) &&
            target.twoFetchStreamId == stream_id &&
            target.twoFetchEpoch == epoch &&
            target.twoFetchPrefetchState != TWO_FETCH_PREFETCH_USED) {
            target.twoFetchPrefetchState = stale ?
                TWO_FETCH_PREFETCH_STALE_RESPONSE :
                TWO_FETCH_PREFETCH_DROPPED;
        }
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_drop", tid, fetch_id,
                             stream_id, epoch, cacheline_base, line_index,
                             "drop", reason, stale);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchCancel(ThreadID tid,
                                                FetchTargetId fetch_id,
                                                uint64_t stream_id,
                                                uint64_t epoch,
                                                Addr cacheline_base,
                                                unsigned line_index,
                                                const char *reason)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchDropped++;
    if (ftq.hasTarget(fetch_id, tid)) {
        auto &target = ftq.get(fetch_id, tid);
        if (isTwoFetchWindow1(target) &&
            target.twoFetchStreamId == stream_id &&
            target.twoFetchEpoch == epoch &&
            target.twoFetchPrefetchState != TWO_FETCH_PREFETCH_USED) {
            target.twoFetchPrefetchState = TWO_FETCH_PREFETCH_CANCELLED;
        }
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_cancel", tid, fetch_id,
                             stream_id, epoch, cacheline_base, line_index,
                             "cancel", reason, false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchBackpressure(ThreadID tid,
                                                      FetchTargetId fetch_id,
                                                      uint64_t stream_id,
                                                      uint64_t epoch,
                                                      Addr cacheline_base,
                                                      unsigned line_index,
                                                      const char *reason)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchBackpressure++;
    dbpBtbStats.twoFetchWindow1IcachePrefetchRetry++;
    if (line_index == 0 && ftq.hasTarget(fetch_id, tid)) {
        auto &target = ftq.get(fetch_id, tid);
        if (isTwoFetchWindow1(target) &&
            target.twoFetchStreamId == stream_id &&
            target.twoFetchEpoch == epoch &&
            !target.twoFetchCostBackpressureAccounted) {
            target.twoFetchCostBackpressureAccounted = true;
            updateTwoFetchUsefulness(
                target, -1,
                TWO_FETCH_USEFULNESS_UPDATE_PREFETCH_BACKPRESSURE);
        }
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_backpressure", tid, fetch_id,
                             stream_id, epoch, cacheline_base, line_index,
                             "drop", reason, false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchDemandPreempt(ThreadID tid,
                                                       FetchTargetId fetch_id,
                                                       uint64_t stream_id,
                                                       uint64_t epoch,
                                                       Addr cacheline_base,
                                                       unsigned line_index,
                                                       const char *reason)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchTooLate++;
    dbpBtbStats.twoFetchWindow1IcachePrefetchDemandPreempt++;
    writeTwoFetchSupplyTrace("two_fetch_prefetch_demand_preempt", tid,
                             fetch_id, stream_id, epoch, cacheline_base,
                             line_index, "demand", reason, false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchDuplicate(ThreadID tid,
                                                   FetchTargetId fetch_id,
                                                   uint64_t stream_id,
                                                   uint64_t epoch,
                                                   Addr cacheline_base,
                                                   unsigned line_index)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchDuplicate++;
    writeTwoFetchSupplyTrace("two_fetch_prefetch_drop", tid, fetch_id,
                             stream_id, epoch, cacheline_base, line_index,
                             "drop", "duplicate", false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchPromotion(ThreadID tid,
                                                   FetchTargetId fetch_id,
                                                   uint64_t stream_id,
                                                   uint64_t epoch,
                                                   Addr cacheline_base,
                                                   const char *reason,
                                                   unsigned bytes,
                                                   bool full)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchHit++;
    if (full) {
        dbpBtbStats.twoFetchWindow1IcachePrefetchPromotionFull++;
    } else {
        dbpBtbStats.twoFetchWindow1IcachePrefetchPromotionPartial++;
    }
    if (ftq.hasTarget(fetch_id, tid)) {
        auto &target = ftq.get(fetch_id, tid);
        if (isTwoFetchWindow1(target) &&
            target.twoFetchStreamId == stream_id &&
            target.twoFetchEpoch == epoch &&
            !target.twoFetchParentInvalidated) {
            target.twoFetchPrefetchState = TWO_FETCH_PREFETCH_USED;
        }
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_promote", tid, fetch_id,
                             stream_id, epoch, cacheline_base, 0,
                             "demand_buffer", reason, false, bytes);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchReadyToHead(Tick ticks)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchReadyToHeadSamples++;
    dbpBtbStats.twoFetchWindow1IcachePrefetchReadyToHeadTicks += ticks;
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchHeadToPromotion(Tick ticks)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchHeadToPromotionSamples++;
    dbpBtbStats.twoFetchWindow1IcachePrefetchHeadToPromotionTicks += ticks;
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchReadyBeforeDemand(
    ThreadID tid, FetchTargetId fetch_id, uint64_t stream_id,
    uint64_t epoch, Addr cacheline_base, unsigned ready_bytes)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchReadyBeforeDemand++;
    writeTwoFetchSupplyTrace("two_fetch_prefetch_ready_before_demand", tid,
                             fetch_id, stream_id, epoch, cacheline_base, 0,
                             "demand", "ready", false, ready_bytes);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchUseful(ThreadID tid,
                                                FetchTargetId fetch_id,
                                                FetchTarget &target)
{
    if (!isTwoFetchWindow1(target) ||
        target.twoFetchPrefetchState != TWO_FETCH_PREFETCH_USED ||
        target.twoFetchParentInvalidated || target.commitInstNum <= 0) {
        return;
    }

    dbpBtbStats.twoFetchWindow1IcachePrefetchUseful++;
    if (!target.twoFetchCostPrefetchUsefulAccounted) {
        target.twoFetchCostPrefetchUsefulAccounted = true;
        updateTwoFetchUsefulness(
            target, 1, TWO_FETCH_USEFULNESS_UPDATE_PREFETCH_USEFUL);
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_useful", tid, fetch_id,
                             target.twoFetchStreamId, target.twoFetchEpoch,
                             target.startPC, 0, "commit", "commit", false,
                             target.commitInstNum);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchPromotionMiss(ThreadID tid,
                                                       FetchTargetId fetch_id,
                                                       uint64_t stream_id,
                                                       uint64_t epoch,
                                                       Addr cacheline_base,
                                                       const char *reason,
                                                       bool stale)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchMiss++;
    dbpBtbStats.twoFetchWindow1IcachePrefetchPromotionMiss++;
    if (stale) {
        dbpBtbStats.twoFetchWindow1IcachePrefetchPromotionStale++;
        dbpBtbStats.twoFetchWindow1IcachePrefetchStaleResponse++;
    } else {
        dbpBtbStats.twoFetchWindow1IcachePrefetchConsumedWithoutReady++;
    }
    if (ftq.hasTarget(fetch_id, tid)) {
        auto &target = ftq.get(fetch_id, tid);
        if (isTwoFetchWindow1(target) &&
            target.twoFetchStreamId == stream_id &&
            target.twoFetchEpoch == epoch) {
            if (stale && target.twoFetchPrefetchState !=
                TWO_FETCH_PREFETCH_USED) {
                target.twoFetchPrefetchState = TWO_FETCH_PREFETCH_STALE_RESPONSE;
            }
            if (!stale && !target.twoFetchCostPromotionMissAccounted) {
                target.twoFetchCostPromotionMissAccounted = true;
                updateTwoFetchUsefulness(
                    target, -1,
                    TWO_FETCH_USEFULNESS_UPDATE_PROMOTION_MISS);
            }
        }
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_drop", tid, fetch_id,
                             stream_id, epoch, cacheline_base, 0,
                             "drop", reason, stale);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchEvict(ThreadID tid,
                                               FetchTargetId fetch_id,
                                               uint64_t stream_id,
                                               uint64_t epoch,
                                               Addr cacheline_base,
                                               bool used,
                                               bool any_ready,
                                               bool cancelled)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchBufferEvicted++;
    if (any_ready && !used) {
        dbpBtbStats.twoFetchWindow1IcachePrefetchReadyButNotConsumed++;
    }
    if (ftq.hasTarget(fetch_id, tid)) {
        auto &target = ftq.get(fetch_id, tid);
        if (isTwoFetchWindow1(target) &&
            target.twoFetchStreamId == stream_id &&
            target.twoFetchEpoch == epoch &&
            target.twoFetchPrefetchState != TWO_FETCH_PREFETCH_USED) {
            target.twoFetchPrefetchState = cancelled ?
                TWO_FETCH_PREFETCH_CANCELLED : TWO_FETCH_PREFETCH_DROPPED;
        }
    }
    writeTwoFetchSupplyTrace("two_fetch_prefetch_drop", tid, fetch_id,
                             stream_id, epoch, cacheline_base, 0,
                             "drop", "buffer_evict", false);
}

void
DecoupledBPUWithBTB::noteTwoFetchPrefetchReadyDiscard(ThreadID tid,
                                                      FetchTargetId fetch_id,
                                                      uint64_t stream_id,
                                                      uint64_t epoch,
                                                      Addr cacheline_base,
                                                      const char *reason)
{
    dbpBtbStats.twoFetchWindow1IcachePrefetchReadyButNotConsumed++;
    dbpBtbStats.twoFetchWindow1IcachePrefetchReadyButCancelled++;
    writeTwoFetchSupplyTrace("two_fetch_prefetch_drop", tid, fetch_id,
                             stream_id, epoch, cacheline_base, 0,
                             "drop", reason, false);
}

void
DecoupledBPUWithBTB::noteTwoFetchOracleFunctionalFill(
    ThreadID tid, FetchTargetId fetch_id, uint64_t stream_id,
    uint64_t epoch, Addr cacheline_base, const char *mode,
    const char *result, unsigned bytes)
{
    const std::string outcome(result ? result : "unknown");
    if (outcome == "attempt") {
        dbpBtbStats.twoFetchOracleFunctionalFillAttempt++;
    } else if (outcome == "success") {
        dbpBtbStats.twoFetchOracleFunctionalFillSuccess++;
        dbpBtbStats.twoFetchOracleFunctionalFillBytes += bytes;
    } else if (outcome == "translation_fault") {
        dbpBtbStats.twoFetchOracleFunctionalFillTranslationFault++;
    } else if (outcome == "invalid_paddr") {
        dbpBtbStats.twoFetchOracleFunctionalFillInvalidPaddr++;
    }
    writeTwoFetchSupplyTrace("two_fetch_oracle_functional_fill", tid,
                             fetch_id, stream_id, epoch, cacheline_base, 0,
                             mode ? mode : "oracle", outcome.c_str(), false,
                             bytes);
}

void
DecoupledBPUWithBTB::noteTwoFetchFrontendOracleDemandFill(
    ThreadID tid, FetchTargetId fetch_id, Addr start_pc,
    const char *result, unsigned bytes)
{
    (void)tid;
    (void)fetch_id;
    (void)start_pc;
    const std::string outcome(result ? result : "unknown");
    if (outcome == "attempt") {
        dbpBtbStats.twoFetchFrontendOracleDemandFillAttempt++;
    } else if (outcome == "success") {
        dbpBtbStats.twoFetchFrontendOracleDemandFillSuccess++;
        dbpBtbStats.twoFetchFrontendOracleDemandFillBytes += bytes;
    } else if (outcome == "translation_fault") {
        dbpBtbStats.twoFetchFrontendOracleDemandFillTranslationFault++;
    } else if (outcome == "invalid_paddr") {
        dbpBtbStats.twoFetchFrontendOracleDemandFillInvalidPaddr++;
    }
}

uint64_t
DecoupledBPUWithBTB::writeTwoAheadRedirectTrace(ThreadID tid,
                                                FetchTargetId target_id,
                                                Addr from_pc,
                                                Addr redirect_pc,
                                                const char *reason)
{
    if ((!enableTwoAheadTwoTakenShadow && !enableTwoAheadTwoTakenFunctional &&
         !enableTwoFetch) ||
        !twoAheadTraceStream) {
        return 0;
    }

    auto &out = *twoAheadTraceStream->stream();
    const auto flush_id = ++twoAheadFlushSeq;
    out << "{\"event\":\"redirect\",\"cycle\":" << curTick()
        << ",\"from_pc\":";
    writeJsonAddr(out, from_pc);
    out << ",\"target\":";
    writeJsonAddr(out, redirect_pc);
    out << ",\"reason\":\"" << reason << "\""
        << ",\"flush_id\":\"f" << flush_id << "\""
        << ",\"metadata_id\":\""
        << shadowMetadataId(tid, target_id, twoAheadEpoch[tid]) << "\""
        << ",\"epoch\":" << twoAheadEpoch[tid] << "}\n";
    out.flush();
    return flush_id;
}

void
DecoupledBPUWithBTB::writeTwoAheadFlushTrace(ThreadID tid,
                                             FetchTargetId begin_id,
                                             FetchTargetId end_id,
                                             uint64_t flush_id,
                                             const char *reason)
{
    if ((!enableTwoAheadTwoTakenShadow && !enableTwoAheadTwoTakenFunctional &&
         !enableTwoFetch) ||
        !twoAheadTraceStream ||
        begin_id > end_id) {
        return;
    }

    auto &out = *twoAheadTraceStream->stream();
    out << "{\"event\":\"flush\",\"cycle\":" << curTick()
        << ",\"flush_id\":\"f" << flush_id << "\""
        << ",\"begin_fetch_id\":" << begin_id
        << ",\"end_fetch_id\":" << end_id
        << ",\"reason\":\"" << reason << "\""
        << ",\"invalidated_metadata_ids\":[";
    for (FetchTargetId id = begin_id; id <= end_id; ++id) {
        if (id != begin_id) {
            out << ",";
        }
        out << "\"" << shadowMetadataId(tid, id, twoAheadEpoch[tid]) << "\"";
    }
    out << "],\"parent_invalidated_metadata_ids\":[";
    bool first_parent_invalidated = true;
    for (FetchTargetId id = begin_id; id <= end_id; ++id) {
        if (!ftq.hasTarget(id, tid)) {
            continue;
        }
        const auto &target = ftq.get(id, tid);
        if (!isTwoFetchWindow1(target) || !target.twoFetchParentInvalidated) {
            continue;
        }
        if (!first_parent_invalidated) {
            out << ",";
        }
        first_parent_invalidated = false;
        out << "\"" << shadowMetadataId(tid, id, twoAheadEpoch[tid]) << "\"";
    }
    out << "]}\n";
    out.flush();
}

bool
DecoupledBPUWithBTB::sharedFTQMode() const
{
    return ftqMode == SMTFTQMode::Shared;
}

unsigned
DecoupledBPUWithBTB::activeFTQThreads() const
{
    if (!sharedFTQMode()) {
        return 1;
    }

    if (!cpu) {
        return std::max(1u, numThreads);
    }

    return std::max(1, cpu->numActiveThreads());
}

unsigned
DecoupledBPUWithBTB::totalFTQEntries() const
{
    unsigned total = 0;
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        total += ftq.size(tid);
    }
    return total;
}

unsigned
DecoupledBPUWithBTB::sharedFTQAllocation(unsigned entries) const
{
    const unsigned active_threads = activeFTQThreads();

    switch (ftqPolicy) {
      case SMTFTQPolicy::Dynamic:
        return entries;
      case SMTFTQPolicy::Partitioned:
        return entries / active_threads;
      case SMTFTQPolicy::Threshold:
        return active_threads == 1 ? entries : std::min(entries, smtFTQThreshold);
      default:
        panic("Invalid SMT FTQ sharing policy");
    }
}

unsigned
DecoupledBPUWithBTB::logicalMaxFTQEntries(ThreadID tid) const
{
    if (!sharedFTQMode()) {
        return ftqEntries;
    }

    return sharedFTQAllocation(ftqEntries);
}

unsigned
DecoupledBPUWithBTB::logicalFreeFTQEntries(ThreadID tid) const
{
    const unsigned local_max = logicalMaxFTQEntries(tid);
    const unsigned local_used = ftq.size(tid);
    const unsigned local_free = local_used >= local_max ? 0 : local_max - local_used;

    if (!sharedFTQMode()) {
        return local_free;
    }

    const unsigned total_used = totalFTQEntries();
    const unsigned shared_free = total_used >= ftqEntries ? 0 : ftqEntries - total_used;
    return std::min(local_free, shared_free);
}

bool
DecoupledBPUWithBTB::ftqFull(ThreadID tid) const
{
    return logicalFreeFTQEntries(tid) == 0;
}

unsigned
DecoupledBPUWithBTB::twoFetchUsefulnessIndex(const FetchTarget &parent,
                                             Addr window1_pc) const
{
    if (twoFetchUsefulnessTable.empty()) {
        return 0;
    }

    const Addr parent_target =
        parent.predTaken ? parent.getTakenTarget() : parent.predEndPC;
    const Addr key = (parent.startPC >> 2) ^ (window1_pc >> 3) ^
        (parent_target >> 5) ^ static_cast<Addr>(parent.asidHash);
    return key % twoFetchUsefulnessTable.size();
}

bool
DecoupledBPUWithBTB::shouldAcceptTwoFetchWindow1(
    const FetchTarget &parent,
    Addr window1_pc,
    bool synthetic_fallback,
    bool predictor_hit,
    uint32_t supply_risk,
    bool usefulness_explored,
    uint32_t &reject_reason) const
{
    reject_reason = TWO_FETCH_REJECT_NONE;
    const ThreadID tid = parent.tid;

    if (window1_pc == MaxAddr) {
        reject_reason |= TWO_FETCH_REJECT_INVALID_PC;
    }

    if (!enableTwoFetchEnhancedGate) {
        return reject_reason == TWO_FETCH_REJECT_NONE;
    }

    if (logicalFreeFTQEntries(tid) < twoFetchGateMinFreeFTQ) {
        reject_reason |= TWO_FETCH_REJECT_FRONTEND_PRESSURE;
    }

    if (threads[tid].squashing || threads[tid].blockPredictionPending) {
        reject_reason |= TWO_FETCH_REJECT_FRONTEND_PRESSURE;
    }

    if (parent.overrideReason != OverrideReason::NO_OVERRIDE) {
        reject_reason |= TWO_FETCH_REJECT_PARENT_RISK;
    }

    if (synthetic_fallback && !enableTwoFetchSyntheticFallback) {
        reject_reason |= TWO_FETCH_REJECT_SYNTHETIC_DISABLED;
    }

    if (!synthetic_fallback && !predictor_hit) {
        reject_reason |= TWO_FETCH_REJECT_PREDICTOR_MISS;
    }

    int usefulness_value = 0;
    bool has_usefulness_value = false;
    if (!twoFetchUsefulnessTable.empty()) {
        const auto idx = twoFetchUsefulnessIndex(parent, window1_pc);
        usefulness_value = twoFetchUsefulnessTable[idx];
        has_usefulness_value = true;
        if (!usefulness_explored &&
            usefulness_value < twoFetchUsefulnessThreshold) {
            reject_reason |= TWO_FETCH_REJECT_USEFULNESS;
        }
    }

    const uint32_t high_recovery_risk =
        TWO_FETCH_SUPPLY_RISK_PARENT_OVERRIDE |
        TWO_FETCH_SUPPLY_RISK_WINDOW1_RETURN |
        TWO_FETCH_SUPPLY_RISK_WINDOW1_INDIRECT |
        TWO_FETCH_SUPPLY_RISK_WINDOW1_CALL |
        TWO_FETCH_SUPPLY_RISK_CROSS_PAGE;
    if ((supply_risk & high_recovery_risk) != 0 &&
        (!has_usefulness_value || usefulness_value <= 0)) {
        reject_reason |= TWO_FETCH_REJECT_SUPPLY_RISK;
    }

    return reject_reason == TWO_FETCH_REJECT_NONE;
}

bool
DecoupledBPUWithBTB::shouldExploreTwoFetchUsefulness(
    ThreadID tid, unsigned usefulness_idx)
{
    dbpBtbStats.twoFetchUsefulnessExploreCandidate++;

    if (twoFetchUsefulnessExploreInterval == 0 ||
        twoFetchUsefulnessExploreCounters.empty()) {
        dbpBtbStats.twoFetchUsefulnessExploreBudgetBlocked++;
        return false;
    }

    if (logicalFreeFTQEntries(tid) < twoFetchUsefulnessExploreMinFreeFTQ) {
        dbpBtbStats.twoFetchUsefulnessExploreResourceBlocked++;
        return false;
    }

    auto &counter = twoFetchUsefulnessExploreCounters[tid];
    const bool allow =
        (counter % twoFetchUsefulnessExploreInterval) == 0;
    counter++;
    if (!allow) {
        dbpBtbStats.twoFetchUsefulnessExploreBudgetBlocked++;
        return false;
    }

    dbpBtbStats.twoFetchUsefulnessExploreAllowed++;
    updateTwoFetchUsefulnessIndex(
        usefulness_idx, 1, TWO_FETCH_USEFULNESS_UPDATE_EXPLORATION_RECOVERY);
    return true;
}

void
DecoupledBPUWithBTB::updateTwoFetchUsefulness(const FetchTarget &target,
                                              int delta,
                                              uint32_t reason)
{
    if (!target.twoFetchMaterialized || target.twoFetchWindowSlot != 1 ||
        twoFetchUsefulnessTable.empty() || delta == 0) {
        return;
    }

    updateTwoFetchUsefulnessIndex(
        target.twoFetchUsefulnessIndex, delta, reason);
}

void
DecoupledBPUWithBTB::updateTwoFetchUsefulnessIndex(unsigned idx, int delta,
                                                   uint32_t reason)
{
    if (twoFetchUsefulnessTable.empty() || delta == 0) {
        return;
    }

    if (delta > 0) {
        dbpBtbStats.twoFetchUsefulnessUpdatePositive++;
    } else {
        dbpBtbStats.twoFetchUsefulnessUpdateNegative++;
    }

    switch (reason) {
      case TWO_FETCH_USEFULNESS_UPDATE_COMMIT:
        dbpBtbStats.twoFetchUsefulnessUpdateRewardCommit++;
        break;
      case TWO_FETCH_USEFULNESS_UPDATE_PREFETCH_USEFUL:
        dbpBtbStats.twoFetchUsefulnessUpdateRewardPrefetchUseful++;
        break;
      case TWO_FETCH_USEFULNESS_UPDATE_PARENT_INVALIDATED:
        dbpBtbStats.twoFetchUsefulnessUpdateCostParentInvalidated++;
        break;
      case TWO_FETCH_USEFULNESS_UPDATE_SELF_SQUASH:
        dbpBtbStats.twoFetchUsefulnessUpdateCostSelfSquash++;
        break;
      case TWO_FETCH_USEFULNESS_UPDATE_WRONG_PATH:
        dbpBtbStats.twoFetchUsefulnessUpdateCostWrongPath++;
        break;
      case TWO_FETCH_USEFULNESS_UPDATE_PREFETCH_BACKPRESSURE:
        dbpBtbStats.twoFetchUsefulnessUpdateCostPrefetchBackpressure++;
        break;
      case TWO_FETCH_USEFULNESS_UPDATE_PROMOTION_MISS:
        dbpBtbStats.twoFetchUsefulnessUpdateCostPromotionMiss++;
        break;
      case TWO_FETCH_USEFULNESS_UPDATE_SUPPLY_RISK_REJECT:
        dbpBtbStats.twoFetchUsefulnessUpdateCostSupplyRiskReject++;
        break;
      case TWO_FETCH_USEFULNESS_UPDATE_EXPLORATION_RECOVERY:
        dbpBtbStats.twoFetchUsefulnessUpdateExplorationRecovery++;
        break;
      default:
        break;
    }

    idx %= twoFetchUsefulnessTable.size();
    const int old_value = twoFetchUsefulnessTable[idx];
    const int max_value = std::max(1, twoFetchUsefulnessMax);
    const int new_value =
        std::clamp(old_value + delta, -max_value, max_value);
    if (new_value != old_value + delta) {
        dbpBtbStats.twoFetchUsefulnessUpdateClamped++;
    }
    twoFetchUsefulnessTable[idx] = static_cast<int8_t>(new_value);
}

ThreadID
DecoupledBPUWithBTB::scheduleThread()
{
    for (ThreadID offset = 0; offset < numThreads; ++offset) {
        const ThreadID tid = (nextPredictTid + offset) % numThreads;

        if (cpu) {
            auto *tc = cpu->getContext(tid);
            if (!tc || tc->status() != gem5::ThreadContext::Active) {
                continue;
            }
        }

        nextPredictTid = (tid + 1) % numThreads;
        return tid;
    }

    return InvalidThreadID;
}


void
DecoupledBPUWithBTB::tick()
{
    DPRINTF(Override, "DecoupledBPUWithBTB::tick()\n");

    ThreadID curTid = scheduleThread();
    if (curTid == InvalidThreadID) {
        return;
    }

    // On squash, reset state if there was a valid prediction.
    bool squashOccurred = false;
    for (int tid = 0; tid < numThreads; tid++) {
        if (threads[tid].squashing) {
            if (tid == curTid) {
                squashOccurred = true;
            }
            threads[tid].validprediction = false;
            threads[tid].numOverrideBubbles = 0;
            tage->dryRunCycle(threads[tid].s0PC);
            DPRINTF(Override, "Squashing, BPU state updated.\n");
            threads[tid].squashing = false;
        }
    }

    if (squashOccurred) {
        DPRINTF(Override, "Squash occurred for current thread, skip predict.\n");
        return;
    }

    // 1. Request new prediction if FSQ not full and we are idle
    if (!threads[curTid].validprediction && !ftqFull(curTid)) {
        if (threads[curTid].blockPredictionPending) {
            DPRINTF(Override, "Prediction blocked to prioritize resolve update\n");
            dbpBtbStats.predictionBlockedForUpdate++;
            threads[curTid].blockPredictionPending = false;
        } else {
            requestNewPrediction(curTid);
        }
    }

    for (int tid = 0; tid < numThreads; tid++) {
        processNewPrediction(tid);

        // Decrement override bubbles counter
        auto& numOverrideBubbles = threads[tid].numOverrideBubbles;
        if (numOverrideBubbles > 0) {
            numOverrideBubbles--;
            dbpBtbStats.overrideBubbleNum++;
            DPRINTF(Override, "Consuming override bubble, %d remaining\n", numOverrideBubbles);
        }
    }

    DPRINTF(Override, "Prediction cycle complete\n");
}

/**
 * @brief Requests new predictions from predictor components
 *
 * If no prediction is in progress and FSQ has space, requests new predictions
 * from each predictor component by sending the current PC and history
 */
void
DecoupledBPUWithBTB::requestNewPrediction(ThreadID tid)
{
    auto& thread = threads[tid];
    auto& predsOfEachStage = threads[tid].predsOfEachStage;
    const uint8_t asid_hash = getThreadAsidHash(tid);

    DPRINTF(Override, "Requesting new prediction for PC %#lx\n", thread.s0PC);

    // Reset all stage-local prediction fields before components fill them.
    clearPreds(tid);
    for (int i = 0; i < numStages; i++) {
        predsOfEachStage[i].tid = tid;
        predsOfEachStage[i].asidHash = asid_hash;
        predsOfEachStage[i].bbStart = thread.s0PC;
        predsOfEachStage[i].predSource = i;
    }

    // Query each predictor component with current PC and history
    for (int i = 0; i < numComponents; i++) {
        components[i]->putPCHistory(thread.s0PC, thread.s0History, predsOfEachStage);  //s0History not used
    }

    generateFinalPredAndCreateBubbles(tid);

    DPRINTF(Override, "Generating final prediction for PC %#lx\n", thread.s0PC);

    threads[tid].validprediction = true;
}

FullBTBPrediction
DecoupledBPUWithBTB::selectFinalPrediction(
    ThreadID tid,
    std::vector<FullBTBPrediction> &preds_of_each_stage,
    bool update_predictor_side_effects,
    bool update_stats)
{
    for (int i = 0; i < numStages; i++) {
        printFullBTBPrediction(preds_of_each_stage[i]);
    }

    FullBTBPrediction *chosenPrediction = &preds_of_each_stage[0];
    for (int i = (int)numStages - 1; i >= 0; i--) {
        if (preds_of_each_stage[i].btbEntries.size() > 0) {
            chosenPrediction = &preds_of_each_stage[i];
            DPRINTF(Override, "Selected prediction from stage %d\n", i);
            break;
        }
    }

    FullBTBPrediction finalPred = *chosenPrediction;
    finalPred.s1Source = -1;
    finalPred.s3Source = -1;

    if (preds_of_each_stage[0].btbEntries.size() != 0) {
        for (auto entry : preds_of_each_stage[0].btbEntries) {
            if (entry.isIndirect || entry.isDirect || entry.ctr >= 0 ||
                entry.alwaysTaken) {
                finalPred.s1Source = entry.source;
                break;
            }
        }
    }

    bool found_s3_taken = false;
    bool na_s3_taken_but_have_cond = false;

    if (numStages > 2) {
        for (BTBEntry entry : preds_of_each_stage[2].btbEntries) {
            if (entry.isDirect || entry.isIndirect || entry.ctr >= 0 ||
                entry.alwaysTaken) {
                found_s3_taken = true;
            } else if (entry.isCond) {
                na_s3_taken_but_have_cond = true;
            }
        }
    }

    if (found_s3_taken) {
        auto pred_taken_entry = finalPred.getTakenEntry();
        if (pred_taken_entry.valid) {
            if (pred_taken_entry.isReturn) {
                finalPred.s3Source = ras->getComponentIdx();
            } else if (pred_taken_entry.isIndirect && ittage->tageHit()) {
                finalPred.s3Source = ittage->getComponentIdx();
            } else if (pred_taken_entry.isCond) {
                finalPred.s3Source = tage->getComponentIdx();
            } else {
                finalPred.s3Source = mbtb->getComponentIdx();
            }
        } else {
            if (na_s3_taken_but_have_cond) {
                finalPred.s3Source = tage->getComponentIdx();
            } else {
                finalPred.s3Source = -1;
            }
        }
    }

    unsigned first_hit_stage = 0;
    OverrideReason overrideReason = OverrideReason::NO_OVERRIDE;

    while (first_hit_stage < numStages - 1) {
        auto [matches, reason] =
            preds_of_each_stage[first_hit_stage].match(*chosenPrediction,
                                                       predictWidth);
        if (matches) {
            break;
        }
        first_hit_stage++;
        overrideReason = reason;
    }

    if (update_predictor_side_effects &&
        preds_of_each_stage[numStages - 1].btbEntries.size() > 0) {
        if (ubtb->isEnabled()) {
            ubtb->updateUsingS3Pred(preds_of_each_stage[numStages - 1]);
        }
        if (abtb->isEnabled() && !ftq.empty(tid)) {
            auto previous_block_startpc = ftq.back(tid).startPC;
            abtb->updateUsingS3Pred(preds_of_each_stage[numStages - 1],
                                    previous_block_startpc);
        } else if (abtb->isEnabled()) {
            abtb->updateUsingS3Pred(preds_of_each_stage[numStages - 1], 0);
        }
    }

    if (update_stats && first_hit_stage > 0) {
        dbpBtbStats.overrideCount++;
    }

    finalPred.predSource = first_hit_stage;
    finalPred.overrideReason = overrideReason;

    printFullBTBPrediction(finalPred);
    if (update_stats) {
        dbpBtbStats.predsOfEachStage[first_hit_stage]++;
    }

    return finalPred;
}

// this function collects predictions from all stages and generate bubbles
// when loop buffer is active, predictions are from saved target
void
DecoupledBPUWithBTB::generateFinalPredAndCreateBubbles(ThreadID tid)
{
    DPRINTF(Override, "In generateFinalPredAndCreateBubbles().\n");

    auto& predsOfEachStage = threads[tid].predsOfEachStage;
    auto& finalPred = threads[tid].finalPred;

    finalPred = selectFinalPrediction(tid, predsOfEachStage, true, true);

    // Clear stage predictions for next cycle
    clearPreds(tid);

    DPRINTF(Override, "Prediction complete: override bubbles=%d\n",
            finalPred.predSource);
    threads[tid].numOverrideBubbles = finalPred.predSource;
}

FullBTBPrediction
DecoupledBPUWithBTB::predictTwoFetchWindow1(ThreadID tid, Addr start_pc)
{
    dbpBtbStats.twoFetchWindow1PredLookup++;

    const uint8_t asid_hash = getThreadAsidHash(tid);
    std::vector<FullBTBPrediction> window1_preds(numStages);
    for (int i = 0; i < numStages; i++) {
        window1_preds[i].tid = tid;
        window1_preds[i].asidHash = asid_hash;
        window1_preds[i].bbStart = start_pc;
        window1_preds[i].predSource = i;
    }

    for (int i = 0; i < numComponents; i++) {
        components[i]->putPCHistory(start_pc, threads[tid].s0History,
                                    window1_preds);
    }

    FullBTBPrediction pred =
        selectFinalPrediction(tid, window1_preds, false, false);
    pred.tid = tid;
    pred.asidHash = asid_hash;
    pred.bbStart = start_pc;

    const bool pred_hit = !pred.btbEntries.empty();
    if (pred_hit) {
        dbpBtbStats.twoFetchWindow1PredHit++;
    }
    if (pred.isTaken()) {
        dbpBtbStats.twoFetchWindow1PredTaken++;
        auto taken_entry = pred.getTakenEntry();
        if (taken_entry.isReturn) {
            dbpBtbStats.twoFetchWindow1PredReturn++;
        }
        if (taken_entry.isIndirect) {
            dbpBtbStats.twoFetchWindow1PredIndirect++;
        }
    }

    return pred;
}

// this function enqueues fsq and update s0PC and s0History
void
DecoupledBPUWithBTB::processNewPrediction(ThreadID tid)
{

    // Check if a prediction is available to enqueue
    if (!threads[tid].validprediction) {
        DPRINTF(Override, "No prediction available to enqueue into FSQ\n");
        return;
    }

    // Check for override bubbles
    // When higher stages override lower stages, bubbles are needed for pipeline consistency
    if (threads[tid].numOverrideBubbles > 0) {
        DPRINTF(Override, "Waiting for %u override bubbles before enqueuing\n", threads[tid].numOverrideBubbles);
        return;
    }

    // Monitor FSQ size for statistics
    dbpBtbStats.fsqEntryDist.sample(ftq.size(tid), 1);
    if (ftqFull(tid)) {
        dbpBtbStats.fsqFullCannotEnq++;
        DPRINTF(Override, "FSQ is full (%lu entries)\n", ftq.size(tid));
        return;
    }

    auto& s0PC = threads[tid].s0PC;

    // Validate PC value
    if (s0PC == MaxAddr) {
        DPRINTF(DecoupleBP, "Invalid PC value %#lx, cannot make prediction\n", s0PC);
        return;
    }

    DPRINTF(DecoupleBP, "Creating new prediction for PC %#lx\n", s0PC);

    // 1. Create a new fetch target entry with prediction information
    FetchTarget entry = createFetchTargetEntry(tid);
    FetchTargetId new_fetch_id = ftq.backId(tid) + 1;
    const bool want_two_fetch = enableTwoFetch;
    if (want_two_fetch) {
        entry.twoFetchBatchId = new_fetch_id;
        entry.twoFetchWindowSlot = 0;
        entry.twoFetchBatchSize = 1;
    }
    const bool can_two_fetch = want_two_fetch && logicalFreeFTQEntries(tid) >= 2;
    if (want_two_fetch && !can_two_fetch) {
        dbpBtbStats.twoFetchFallbackFtqCapacity++;
        dbpBtbStats.twoFetchWindow1RejectedFtqCapacity++;
        entry.twoFetchRejectReason |= TWO_FETCH_REJECT_FTQ_CAPACITY;
    }

    // 2. Update global PC state to target or fall-through
    const Addr window1_pc = threads[tid].finalPred.getTarget(predictWidth);

    // 3. Update history information
    updateHistoryForPrediction(entry);

    // 4. Fill ahead pipeline
    fillAheadPipeline(entry, new_fetch_id);

    FetchTarget window1_entry;
    FullBTBPrediction window1_pred;
    FetchTargetId window1_fetch_id = new_fetch_id + 1;
    const bool valid_window1_pc = window1_pc != MaxAddr;
    if (can_two_fetch && !valid_window1_pc) {
        dbpBtbStats.twoFetchFallbackInvalidPC++;
        dbpBtbStats.twoFetchWindow1RejectedInvalidPC++;
        entry.twoFetchRejectReason |= TWO_FETCH_REJECT_INVALID_PC;
    }

    bool materialize_window1 = false;
    bool window1_lookup_done = false;
    uint32_t gate_reject_reason = TWO_FETCH_REJECT_NONE;
    bool window1_usefulness_explored = false;
    if (can_two_fetch && valid_window1_pc) {
        dbpBtbStats.twoFetchWindow1Candidate++;
        entry.twoFetchUsefulnessIndex =
            twoFetchUsefulnessIndex(entry, window1_pc);

        if (enableTwoFetchEnhancedGate) {
            if (logicalFreeFTQEntries(tid) < twoFetchGateMinFreeFTQ) {
                gate_reject_reason |= TWO_FETCH_REJECT_FRONTEND_PRESSURE;
            }
            if (threads[tid].squashing || threads[tid].blockPredictionPending) {
                gate_reject_reason |= TWO_FETCH_REJECT_FRONTEND_PRESSURE;
            }
            if (entry.overrideReason != OverrideReason::NO_OVERRIDE) {
                gate_reject_reason |= TWO_FETCH_REJECT_PARENT_RISK;
            }
            if (!twoFetchUsefulnessTable.empty()) {
                const int usefulness_value =
                    twoFetchUsefulnessTable[entry.twoFetchUsefulnessIndex];
                if (usefulness_value < twoFetchUsefulnessThreshold) {
                    gate_reject_reason |= TWO_FETCH_REJECT_USEFULNESS;
                }
            }
            const uint32_t hard_prelookup_rejects =
                gate_reject_reason & ~TWO_FETCH_REJECT_USEFULNESS;
            if ((gate_reject_reason & TWO_FETCH_REJECT_USEFULNESS) != 0 &&
                hard_prelookup_rejects == TWO_FETCH_REJECT_NONE &&
                shouldExploreTwoFetchUsefulness(
                    tid, entry.twoFetchUsefulnessIndex)) {
                gate_reject_reason &= ~TWO_FETCH_REJECT_USEFULNESS;
                window1_usefulness_explored = true;
            }
        }

        if (gate_reject_reason != TWO_FETCH_REJECT_NONE) {
            dbpBtbStats.twoFetchWindow1PreLookupRejected++;
        }

        if (gate_reject_reason == TWO_FETCH_REJECT_NONE) {
            window1_pred = predictTwoFetchWindow1(tid, window1_pc);
            window1_lookup_done = true;
            window1_entry =
                createTwoFetchWindow1Entry(tid, entry, new_fetch_id, window1_pc,
                                           window1_pred);
            const uint32_t window1_supply_risk =
                twoFetchSupplyRisk(entry, window1_entry);
            window1_entry.twoFetchSupplyRisk = window1_supply_risk;
            const bool synthetic_fallback = false;
            materialize_window1 = shouldAcceptTwoFetchWindow1(
                entry, window1_pc, synthetic_fallback, window1_entry.isHit,
                window1_supply_risk, window1_usefulness_explored,
                gate_reject_reason);
        }

        if (!materialize_window1) {
            if (window1_lookup_done) {
                dbpBtbStats.twoFetchWindow1PostLookupRejected++;
            }
            if ((gate_reject_reason & TWO_FETCH_REJECT_SUPPLY_RISK) != 0 &&
                window1_lookup_done) {
                updateTwoFetchUsefulnessIndex(
                    entry.twoFetchUsefulnessIndex, -1,
                    TWO_FETCH_USEFULNESS_UPDATE_SUPPLY_RISK_REJECT);
            }
            entry.twoFetchRejectReason = gate_reject_reason;
            entry.twoFetchPredictionKind =
                TWO_FETCH_PRED_PREDICTOR_BACKED_CANDIDATE;
            entry.twoFetchPredictorBacked = window1_lookup_done;
            entry.twoFetchPredictionAccepted = false;
            entry.twoFetchPredHit = window1_entry.isHit;
            entry.twoFetchPredTaken = window1_entry.predTaken;
            entry.twoFetchPredReturn =
                window1_entry.predTaken && window1_entry.predBranchInfo.isReturn;
            entry.twoFetchPredIndirect =
                window1_entry.predTaken && window1_entry.predBranchInfo.isIndirect;
            dbpBtbStats.twoFetchWindow1RejectedGate++;
            if (gate_reject_reason & TWO_FETCH_REJECT_SYNTHETIC_DISABLED) {
                dbpBtbStats.twoFetchWindow1RejectedSyntheticFallback++;
            }
            if (gate_reject_reason & TWO_FETCH_REJECT_USEFULNESS) {
                dbpBtbStats.twoFetchWindow1RejectedUsefulness++;
            }
            if (gate_reject_reason & TWO_FETCH_REJECT_PARENT_RISK) {
                dbpBtbStats.twoFetchWindow1RejectedParentRisk++;
            }
            if (gate_reject_reason & TWO_FETCH_REJECT_FRONTEND_PRESSURE) {
                dbpBtbStats.twoFetchWindow1RejectedFrontendPressure++;
            }
            if (gate_reject_reason & TWO_FETCH_REJECT_PREDICTOR_MISS) {
                dbpBtbStats.twoFetchWindow1RejectedPredMiss++;
            }
            if (gate_reject_reason & TWO_FETCH_REJECT_SUPPLY_RISK) {
                dbpBtbStats.twoFetchWindow1RejectedSupplyRisk++;
            }
        }
    }

    if (materialize_window1) {
        const Tick materialize_tick = curTick();
        entry.twoFetchMaterialized = true;
        entry.twoFetchBatchId = new_fetch_id;
        entry.twoFetchWindowSlot = 0;
        entry.twoFetchBatchSize = 2;
        entry.twoFetchParentFetchId = 0;
        entry.twoFetchEpoch = entry.shadowEpoch;
        entry.twoFetchMaterializeTick = materialize_tick;
        entry.twoFetchPredictionKind =
            TWO_FETCH_PRED_PREDICTOR_BACKED_ACCEPTED;
        entry.twoFetchPredictorBacked = true;
        entry.twoFetchPredictionAccepted = true;
        entry.twoFetchPredHit = entry.isHit;
        entry.twoFetchPredTaken = entry.predTaken;
        entry.twoFetchPredReturn =
            entry.predTaken && entry.predBranchInfo.isReturn;
        entry.twoFetchPredIndirect =
            entry.predTaken && entry.predBranchInfo.isIndirect;
        entry.twoFetchUsefulnessIndex =
            twoFetchUsefulnessIndex(entry, window1_pc);
        window1_entry.twoFetchMaterialized = true;
        window1_entry.twoFetchBatchId = new_fetch_id;
        window1_entry.twoFetchWindowSlot = 1;
        window1_entry.twoFetchBatchSize = 2;
        window1_entry.twoFetchParentFetchId = new_fetch_id;
        window1_entry.twoFetchStreamId =
            twoFetchStreamId(tid, window1_fetch_id);
        window1_entry.twoFetchEpoch = window1_entry.shadowEpoch;
        window1_entry.twoFetchMaterializeTick = materialize_tick;
        window1_entry.twoFetchPrefetchState =
            TWO_FETCH_PREFETCH_NOT_ISSUED;
        window1_entry.twoFetchSupplyRisk =
            twoFetchSupplyRisk(entry, window1_entry);
        window1_entry.twoFetchPredictionKind =
            TWO_FETCH_PRED_PREDICTOR_BACKED_ACCEPTED;
        window1_entry.twoFetchPredictionAccepted = true;
        window1_entry.twoFetchUsefulnessIndex =
            entry.twoFetchUsefulnessIndex;
        accountTwoFetchWindow1HistorySnapshot(window1_entry);
        fillAheadPipeline(window1_entry, window1_fetch_id,
                          &entry, new_fetch_id);
        updateHistoryForPrediction(window1_entry, window1_pred,
                                   window1_fetch_id);
        s0PC = window1_pred.getTarget(predictWidth);
        dbpBtbStats.twoFetchBatchCreated++;
        dbpBtbStats.twoFetchWindow1Materialized++;
        dbpBtbStats.twoFetchWindow1Accepted++;
        if (window1_usefulness_explored) {
            dbpBtbStats.twoFetchWindow1AcceptedByExploration++;
        }
        dbpBtbStats.twoFetchWindow1StreamCreated++;
        dbpBtbStats.twoFetchWindow1StreamAccepted++;
    } else {
        s0PC = window1_pc;
    }

    writeTwoAheadPredictTrace(
        tid, new_fetch_id, entry, threads[tid].finalPred,
        materialize_window1 ? &window1_entry : nullptr,
        materialize_window1 ? window1_fetch_id : 0);

    if (enablePredFSQTrace) {
        predTraceManager->write_record(PredictionTrace(ftq.backId(tid), entry));
    }

    // 5. Add entry to fetch target queue
    ftq.insert(entry);
    if (materialize_window1) {
        ftq.insert(window1_entry);
        dbpBtbStats.twoFetchWindow1Enqueued++;
    }
    threads[tid].validprediction = false;

    // 6. Debug output and update statistics
    dumpFsq("after insert new target");
    DPRINTF(DecoupleBP, "Inserted fetch target %lu starting at PC %#lx\n",
            ftq.backId(tid), entry.startPC);

    // 7. Increment statistics
    printTarget(entry);
    dbpBtbStats.fsqEntryEnqueued++;
    if (materialize_window1) {
        dbpBtbStats.fsqEntryEnqueued++;
    }
}

/**
 * @brief Common logic for handling squash events
 *
 * This function encapsulates the shared logic between different types of squashes:
 * - Setting squashing state
 * - Finding and updating the target
 * - Recovering history information
 * - Clearing predictions
 * - Updating FTQ and FSQ state
 *
 * @param target_id ID of the target being squashed
 * @param squash_type Type of squash (CTRL/OTHER/TRAP)
 * @param squash_pc PC where the squash occurred
 * @param redirect_pc PC to redirect to after squash
 * @param is_conditional Whether the squash is caused by a conditional branch
 * @param actually_taken Whether the branch was actually taken (for conditional branches)
 * @param static_inst Static instruction pointer (for control squash)
 * @param control_inst_size Size of the control instruction (for control squash)
 */
void
DecoupledBPUWithBTB::handleSquash(ThreadID tid, unsigned target_id,
                                 SquashType squash_type,
                                 const PCStateBase &squash_pc,
                                 Addr redirect_pc,
                                 bool is_conditional,
                                 bool actually_taken,
                                 const StaticInstPtr &static_inst,
                                 unsigned control_inst_size)
{
    // Set squashing state
    threads[tid].squashing = true;

    // Find the target being squashed
    if (!ftq.hasTarget(target_id, tid)) {
        DPRINTF(DecoupleBP,
                "Ignore squash for tid %u on missing FTQ target %u; "
                "recovering predictor state from redirect PC %#lx\n",
                tid, target_id, redirect_pc);
        if (!ftq.empty(tid)) {
            const auto flush_id = ++twoAheadFlushSeq;
            for (FetchTargetId id = ftq.frontId(tid); id <= ftq.backId(tid); ++id) {
                auto &invalidated = ftq.get(id, tid);
                if (invalidated.twoFetchMaterialized &&
                    invalidated.twoFetchWindowSlot == 1) {
                    accountTwoFetchWindow1HistoryRestore(invalidated);
                    dbpBtbStats.twoFetchWindow1Squashed++;
                    if (!invalidated.twoFetchConsumed) {
                        dbpBtbStats.twoFetchWindow1SquashedBeforeFetch++;
                    } else {
                        dbpBtbStats.twoFetchWindow1WrongPathFetchInsts +=
                            invalidated.twoFetchConsumedInsts;
                    }
                    invalidated.twoFetchPrefetchState =
                        TWO_FETCH_PREFETCH_CANCELLED;
                    dbpBtbStats.twoFetchWindow1StreamInvalidated++;
                    updateTwoFetchUsefulness(
                        invalidated, -1,
                        invalidated.twoFetchConsumed ?
                            TWO_FETCH_USEFULNESS_UPDATE_WRONG_PATH :
                            TWO_FETCH_USEFULNESS_UPDATE_SELF_SQUASH);
                }
            }
            writeTwoAheadFlushTrace(tid, ftq.frontId(tid), ftq.backId(tid),
                                    flush_id, "missing_target_squash");
        }
        twoAheadEpoch[tid]++;
        ftq.clear(tid);
        clearPreds(tid);
        threads[tid].validprediction = false;
        threads[tid].s0PC = redirect_pc;
        return;
    }

    // Get reference to the target
    auto &target = ftq.get(target_id, tid);
    const FetchTargetId old_back_id = ftq.backId(tid);

    // Update target state
    target.resolved = true;
    target.exeTaken = actually_taken;
    target.squashPC = squash_pc.instAddr();
    target.squashType = squash_type;
    if (target.twoFetchMaterialized && target.twoFetchWindowSlot == 1) {
        accountTwoFetchWindow1HistoryRestore(target);
        dbpBtbStats.twoFetchWindow1Squashed++;
        dbpBtbStats.twoFetchWindow1SquashedBySelf++;
        if (!target.twoFetchConsumed) {
            dbpBtbStats.twoFetchWindow1SquashedBeforeFetch++;
        } else {
            dbpBtbStats.twoFetchWindow1WrongPathFetchInsts +=
                target.twoFetchConsumedInsts;
        }
        target.twoFetchPrefetchState = TWO_FETCH_PREFETCH_CANCELLED;
        dbpBtbStats.twoFetchWindow1StreamInvalidated++;
        dbpBtbStats.twoFetchWindow1StreamSelfInvalidated++;
        updateTwoFetchUsefulness(
            target, -1,
            target.twoFetchConsumed ?
                TWO_FETCH_USEFULNESS_UPDATE_WRONG_PATH :
                TWO_FETCH_USEFULNESS_UPDATE_SELF_SQUASH);
    }

    // Special handling for control squash - create branch info
    if (squash_type == SQUASH_CTRL && static_inst) {
        // Use full branch info with static_inst if available
        target.exeBranchInfo = BranchInfo(squash_pc.instAddr(), redirect_pc, static_inst, control_inst_size);
        dumpFsq("Before control squash");
    }

    const auto flush_id = writeTwoAheadRedirectTrace(
        tid, target_id, squash_pc.instAddr(), redirect_pc, "squash");
    for (FetchTargetId id = target_id + 1; id <= old_back_id; ++id) {
        auto &invalidated = ftq.get(id, tid);
        if (invalidated.twoFetchMaterialized &&
            invalidated.twoFetchWindowSlot == 1) {
            accountTwoFetchWindow1HistoryRestore(invalidated);
            dbpBtbStats.twoFetchWindow1Squashed++;
            if (!invalidated.twoFetchConsumed) {
                dbpBtbStats.twoFetchWindow1SquashedBeforeFetch++;
            } else {
                dbpBtbStats.twoFetchWindow1WrongPathFetchInsts +=
                    invalidated.twoFetchConsumedInsts;
            }
            if (target.twoFetchMaterialized &&
                target.twoFetchWindowSlot == 0 &&
                invalidated.twoFetchParentFetchId == target_id) {
                invalidated.twoFetchParentInvalidated = true;
                invalidated.twoFetchPrefetchState =
                    TWO_FETCH_PREFETCH_CANCELLED;
                dbpBtbStats.twoFetchWindow1ParentInvalidated++;
                dbpBtbStats.twoFetchWindow1SquashedByWindow0++;
                dbpBtbStats.twoFetchWindow1StreamInvalidated++;
                dbpBtbStats.twoFetchWindow1StreamParentInvalidated++;
                updateTwoFetchUsefulness(
                    invalidated, -2,
                    TWO_FETCH_USEFULNESS_UPDATE_PARENT_INVALIDATED);
            } else {
                invalidated.twoFetchPrefetchState =
                    TWO_FETCH_PREFETCH_CANCELLED;
                dbpBtbStats.twoFetchWindow1SquashedBySelf++;
                dbpBtbStats.twoFetchWindow1StreamInvalidated++;
                dbpBtbStats.twoFetchWindow1StreamSelfInvalidated++;
                updateTwoFetchUsefulness(
                    invalidated, -1,
                    invalidated.twoFetchConsumed ?
                        TWO_FETCH_USEFULNESS_UPDATE_WRONG_PATH :
                        TWO_FETCH_USEFULNESS_UPDATE_SELF_SQUASH);
            }
        }
    }
    writeTwoAheadFlushTrace(tid, target_id + 1, old_back_id, flush_id,
                            "squash");
    twoAheadEpoch[tid]++;

    // Remove targets after the squashed one
    ftq.squashAfter(target_id, tid);

    // Recover history using the extracted function
    recoverHistoryForSquash(target, target_id, squash_pc, is_conditional, actually_taken, squash_type, redirect_pc);

    // Clear predictions for next cycle
    clearPreds(tid);

    // Update PC and target ID
    threads[tid].s0PC = redirect_pc;

    DPRINTF(DecoupleBP,
            "After squash, fsqId(next alloc)=%lu, fetchHeadFsqId=%lu, s0pc=%#lx\n",
            ftq.backId(tid) + 1, ftq.frontId(tid), redirect_pc);
}

void
DecoupledBPUWithBTB::controlSquash(unsigned target_id,
                            const PCStateBase &control_pc,
                            const PCStateBase &corr_target,
                            const StaticInstPtr &static_inst,
                            unsigned control_inst_size, bool actually_taken,
                            const InstSeqNum &seq, ThreadID tid,
                            const unsigned &currentLoopIter, const bool fromCommit)
{
    if (fromCommit) {
        dbpBtbStats.controlSquashFromCommit++;
        auto branchClass = classifyBranch(static_inst);
        addControlSquashCommitStat(branchClass);
    } else {
        dbpBtbStats.controlSquashFromDecode++;
    }

    // Get branch type information
    bool is_conditional = static_inst->isCondCtrl();
    bool is_indirect = static_inst->isIndirectCtrl();

    if (!ftq.hasTarget(target_id, tid)) {
        DPRINTF(DecoupleBP, "The squashing target is insane, ignore squash on it");
        return;
    }
    auto &target = ftq.get(target_id, tid);
    // Get target address
    Addr real_target = corr_target.instAddr();
    if (!fromCommit && static_inst->isReturn() && !static_inst->isNonSpeculative()) {
        // get ret addr from ras meta
        real_target = ras->getTopAddrFromMetas(target);
        // TODO: set real target to dynamic inst
    }

    // Detailed debugging for control squash
    DPRINTF(DecoupleBP,
            "Control squash: ftq_id=%d,"
            " control_pc=%#lx, real_target=%#lx, is_conditional=%u, "
            "is_indirect=%u, actually_taken=%u, branch seq: %lu\n",
            target_id, control_pc.instAddr(),
            real_target, is_conditional, is_indirect,
            actually_taken, seq);

    // Call shared squash handling logic
    handleSquash(tid, target_id, SQUASH_CTRL, control_pc,
                real_target, is_conditional, actually_taken, static_inst, control_inst_size);
}

void
DecoupledBPUWithBTB::nonControlSquash(unsigned target_id,
                               const PCStateBase &inst_pc,
                               const InstSeqNum seq, ThreadID tid, const unsigned &currentLoopIter)
{
    dbpBtbStats.nonControlSquash++;
    DPRINTF(DecoupleBP,
            "non control squash: target id: %d, inst_pc: %#lx, "
            "seq: %lu\n",
            target_id, inst_pc.instAddr(), seq);

    // Call shared squash handling logic
    handleSquash(tid, target_id, SQUASH_OTHER, inst_pc, inst_pc.instAddr());
}

void
DecoupledBPUWithBTB::trapSquash(unsigned target_id,
                         Addr last_committed_pc, const PCStateBase &inst_pc,
                         ThreadID tid, const unsigned &currentLoopIter)
{
    dbpBtbStats.trapSquash++;
    DPRINTF(DecoupleBP,
            "Trap squash: target id: %d, inst_pc: %#lx\n",
            target_id, inst_pc.instAddr());

    // Call shared squash handling logic
    handleSquash(tid, target_id, SQUASH_TRAP, inst_pc, inst_pc.instAddr());
}

void
DecoupledBPUWithBTB::commit(unsigned target_id, ThreadID tid)
{
    // No need to dequeue when queue is empty
    if (ftq.empty(tid)) {
        return;
    }

    // Process all targets that have been committed (target_id >= head target id).
    while (!ftq.empty(tid) && target_id >= ftq.frontId(tid)) {
        auto &target = ftq.front(tid);

        DPRINTF(DecoupleBP,
                "Commit target start %#lx, which is predicted, "
                "final br addr: %#lx, final target: %#lx, pred br addr: %#lx, "
                "pred target: %#lx\n",
                target.startPC, target.exeBranchInfo.pc, target.exeBranchInfo.target, target.predBranchInfo.pc,
                target.predBranchInfo.target);

        // Update statistics
        updateStatistics(target);
        noteTwoFetchPrefetchUseful(tid, ftq.frontId(tid), target);

        writeTwoAheadLifecycleTrace("resolve", tid, ftq.frontId(tid), target);

        if (target.twoFetchParentInvalidated) {
            dbpBtbStats.twoFetchWindow1ParentInvalidatedTrainBlocked++;
        } else {
            writeTwoAheadLifecycleTrace("train", tid, ftq.frontId(tid), target);
            updatePredictorComponents(target);
        }

        writeTwoAheadLifecycleTrace("commit", tid, ftq.frontId(tid), target);

        ftq.commitTarget(tid);
        dbpBtbStats.fsqEntryCommitted++;
    }

    DPRINTF(DecoupleBP, "after commit target, fetchTargetQueue size: %lu\n", ftq.size(tid));

    if (!ftq.empty(tid))
        printTarget(ftq.front(tid));

    historyManagers[tid].commit(target_id);
}

bool
DecoupledBPUWithBTB::resolveUpdate(unsigned &target_id, ThreadID tid)
{
    if (!ftq.hasTarget(target_id, tid)) {
        DPRINTF(DecoupleBP, "Target id %u not found in fetchTargetQueue, cannot update predictors\n", target_id);
        return true;
    }

    auto &target = ftq.get(target_id, tid);
    if (target.twoFetchParentInvalidated) {
        dbpBtbStats.twoFetchWindow1ParentInvalidatedTrainBlocked++;
        return true;
    }

    // Update predictor components only if the target is hit or taken
    if (!(target.isHit || target.exeTaken)) {
        return true;
    }

    // Phase 1: probe all resolved-update components to ensure no blocker
    for (int i = 0; i < numComponents; ++i) {
        if (components[i]->getResolvedUpdate()) {
            if (!components[i]->canResolveUpdate(target)) {
                return false;
            }
        }
    }

    // Phase 2: all clear, perform updates once
    for (int i = 0; i < numComponents; ++i) {
        if (components[i]->getResolvedUpdate()) {
            components[i]->doResolveUpdate(target);
        }
    }

    return true;
}

void
DecoupledBPUWithBTB::notifyResolveSuccess(ThreadID tid)
{
    resolveDequeueFailCounters[tid] = 0;
}

void
DecoupledBPUWithBTB::notifyResolveFailure(ThreadID tid)
{
    auto &failCounter = resolveDequeueFailCounters[tid];
    failCounter++;
    if (failCounter >= resolveBlockThreshold) {
        blockPredictionOnce(tid);
        failCounter = 0;
    }
}

void
DecoupledBPUWithBTB::blockPredictionOnce(ThreadID tid)
{
    threads[tid].blockPredictionPending = true;
}

void
DecoupledBPUWithBTB::prepareResolveUpdateEntries(unsigned &target_id, ThreadID tid)
{
    if (!ftq.hasTarget(target_id, tid)) {
        DPRINTF(DecoupleBP, "Target id %u not found in fetchTargetQueue, cannot update predictors\n", target_id);
        return;
    }
    auto &target = ftq.get(target_id, tid);

    if (target.isHit || target.exeTaken) {
        // Prepare target for update
        target.setUpdateInstEndPC(predictWidth);
        target.setUpdateBTBEntries();

        // only mbtb can generate new entry
        if (mbtb->isEnabled()) {
            mbtb->getAndSetNewBTBEntry(target);
        }
    }
}

void
DecoupledBPUWithBTB::markCFIResolved(unsigned &target_id, uint64_t resolvedInstPC, ThreadID tid)
{

    if (!ftq.hasTarget(target_id, tid)) {
        DPRINTF(DecoupleBP, "Target id %u not found in fetchTargetQueue, cannot update predictors\n", target_id);
        return;
    }
    auto &target = ftq.get(target_id, tid);

    if (target.updateNewBTBEntry.pc == resolvedInstPC) {
        target.updateNewBTBEntry.resolved = true;
    }

    target.markBTBEntryResolved(resolvedInstPC);
}

void
DecoupledBPUWithBTB::updatePredictorComponents(FetchTarget &target)
{
    if (target.twoFetchParentInvalidated) {
        dbpBtbStats.twoFetchWindow1ParentInvalidatedTrainBlocked++;
        return;
    }

    // Update predictor components only if the target is hit or taken
    if (target.isHit || target.exeTaken) {
        // Prepare target for update
        target.setUpdateInstEndPC(predictWidth);
        target.setUpdateBTBEntries();

        // only mbtb can generate new entry
        if (mbtb->isEnabled()) {
            mbtb->getAndSetNewBTBEntry(target);
        }

        // Update predictor components
        for (int i = 0; i < numComponents; ++i) {
            if (!components[i]->getResolvedUpdate()) {
                components[i]->update(target);
            }
        }
    }
}


void
DecoupledBPUWithBTB::histShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history)
{
    if (shamt == 0) {
        return;
    }
    history <<= shamt;
    history[0] = taken;
}

void
DecoupledBPUWithBTB::pHistShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history, Addr pc, Addr target)
{
    if (shamt == 0) {
        return;
    }
    if(taken){
        // Calculate path hash
        uint64_t hash = pathHash(pc, target);

        history <<= shamt;
        for (auto i = 0; i < pathHashLength && i < history.size(); i++) {
            history[i] = (hash & 1) ^ history[i];
            hash >>= 1;
        }
    }
}

/**
 * @brief Creates a new FetchTarget entry with prediction information
 *
 * @return FetchTarget The created fetch target
 */
FetchTarget
DecoupledBPUWithBTB::createFetchTargetFromPrediction(ThreadID tid,
                                                     Addr start_pc,
                                                     FullBTBPrediction &pred,
                                                     uint64_t shadow_epoch)
{
    auto& s0History = threads[tid].s0History;
    auto& s0PHistory = threads[tid].s0PHistory;
    auto& s0BwHistory = threads[tid].s0BwHistory;
    auto& s0LHistory = threads[tid].s0LHistory;

    // Create a new fetch target entry
    FetchTarget entry;
    entry.tid = tid;
    entry.asidHash = pred.asidHash;
    entry.startPC = start_pc;

    // Extract branch prediction information
    bool taken = pred.isTaken();
    Addr fallThroughAddr = pred.getFallThrough(predictWidth);
    Addr nextPC = pred.getTarget(predictWidth);

    // Configure target entry with prediction details
    entry.isHit = !pred.btbEntries.empty();
    entry.falseHit = false;
    entry.predBTBEntries = pred.btbEntries;
    entry.predTaken = taken;
    entry.predEndPC = fallThroughAddr;

    // Set branch info for taken predictions
    if (taken) {
        entry.predBranchInfo = pred.getTakenEntry().getBranchInfo();
        entry.predBranchInfo.target = nextPC; // Use final target (may not be from BTB)
    }

    // Record current history and prediction metadata
    entry.history = s0History;
    entry.phistory = s0PHistory;
    entry.bwhistory = s0BwHistory;
    entry.lhistory = s0LHistory;
    entry.predTick = pred.predTick;
    entry.predSource = pred.predSource;
    entry.overrideReason = pred.overrideReason;
    entry.shadowEpoch = shadow_epoch;

    entry.s1Source = pred.s1Source;
    entry.s3Source = pred.s3Source;

    // Save predictors' metadata
    for (int i = 0; i < numComponents; i++) {
        entry.predMetas[i] = components[i]->getPredictionMeta(tid);
    }

    // Initialize default resolution state
    entry.setDefaultResolve();

    return entry;
}

FetchTarget
DecoupledBPUWithBTB::createFetchTargetEntry(ThreadID tid)
{
    return createFetchTargetFromPrediction(
        tid, threads[tid].s0PC, threads[tid].finalPred, twoAheadEpoch[tid]);
}

FetchTarget
DecoupledBPUWithBTB::createTwoFetchWindow1Entry(ThreadID tid,
                                                const FetchTarget &parent,
                                                FetchTargetId parent_id,
                                                Addr start_pc,
                                                FullBTBPrediction &pred)
{
    FetchTarget entry =
        createFetchTargetFromPrediction(tid, start_pc, pred,
                                        parent.shadowEpoch);
    entry.twoAheadWindowCount = 1;
    entry.twoAheadWindowValid[0] = true;
    entry.twoAheadWindowPC[0] = start_pc;
    entry.twoAheadWindowFallthrough[0] = entry.predEndPC;
    entry.twoFetchBatchId = parent_id;
    entry.twoFetchWindowSlot = 1;
    entry.twoFetchBatchSize = 2;
    entry.twoFetchParentFetchId = parent_id;
    entry.twoFetchPredictionKind = TWO_FETCH_PRED_PREDICTOR_BACKED_CANDIDATE;
    entry.twoFetchPredictorBacked = true;
    entry.twoFetchPredictionAccepted = false;
    entry.twoFetchPredHit = entry.isHit;
    entry.twoFetchPredTaken = entry.predTaken;
    entry.twoFetchPredReturn =
        entry.predTaken && entry.predBranchInfo.isReturn;
    entry.twoFetchPredIndirect =
        entry.predTaken && entry.predBranchInfo.isIndirect;
    entry.twoFetchUsefulnessIndex =
        twoFetchUsefulnessIndex(parent, start_pc);
    return entry;
}

/**
 * @brief fill ahead pipeline entry.previousPCs
 */
void
DecoupledBPUWithBTB::fillAheadPipeline(FetchTarget &entry,
                                       FetchTargetId fetch_id,
                                       const FetchTarget *pending_target,
                                       FetchTargetId pending_target_id)
{
    ThreadID tid = entry.tid;
    // Handle ahead pipelined predictors
    unsigned max_ahead_pipeline_stages = 0;
    for (int i = 0; i < numComponents; i++) {
        max_ahead_pipeline_stages = std::max(max_ahead_pipeline_stages, components[i]->aheadPipelinedStages);
    }

    // Get previous PCs from fetchTargetQueue if needed
    if (max_ahead_pipeline_stages > 0) {
        for (int i = 0; i < max_ahead_pipeline_stages; i++) {
            if (fetch_id + i < max_ahead_pipeline_stages) {
                continue;
            }
            const FetchTargetId id =
                fetch_id - max_ahead_pipeline_stages + i;
            if (pending_target && id == pending_target_id) {
                entry.previousPCs.push(pending_target->getRealStartPC());
            } else if (ftq.hasTarget(id, tid)) {
                // FIXME: it may not work well with jump ahead predictor
                entry.previousPCs.push(ftq.get(id, tid).getRealStartPC());
            }
        }
    }
}

void
DecoupledBPUWithBTB::checkHistories(const boost::dynamic_bitset<> &history,
                                    const boost::dynamic_bitset<> &phistory,
                                    ThreadID tid)
{
    DPRINTF(DecoupleBP, "Checking GHR/PHR speculative history replay\n");
    assert(historyManagers[tid].checkGHist(history, historyBits));
    assert(historyManagers[tid].checkPHist(phistory, historyBits));
}

void
DecoupledBPUWithBTB::resetPC(Addr new_pc)
{
    for (int i = 0; i < numThreads; i++)
        threads[i].s0PC = new_pc;
}

void
DecoupledBPUWithBTB::resetPC(ThreadID tid, Addr new_pc)
{
    threads[tid].s0PC = new_pc;
}

Addr
DecoupledBPUWithBTB::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    DPRINTF(DecoupleBP, "acquiring reutrn address for inst pc %#lx from decode\n", dynInst->pcState().instAddr());
    auto ftqid = dynInst->getFtqId();
    auto retAddr = ras->getTopAddrFromMetas(ftq.get(ftqid, dynInst->threadNumber));
    DPRINTF(DecoupleBP, "get ret addr %#lx\n", retAddr);
    return retAddr;
}

/**
 * @brief Updates global history based on prediction results
 *
 * @param entry The fetch target entry to update history for
 */
void
DecoupledBPUWithBTB::updateHistoryForPrediction(FetchTarget &entry)
{
    updateHistoryForPrediction(entry, threads[entry.tid].finalPred,
                               ftq.backId(entry.tid) + 1);
}

void
DecoupledBPUWithBTB::updateHistoryForPrediction(FetchTarget &entry,
                                                FullBTBPrediction &pred,
                                                FetchTargetId fetch_id)
{
    ThreadID tid = entry.tid;
    auto& s0History = threads[tid].s0History;
    auto& s0PHistory = threads[tid].s0PHistory;
    auto& s0BwHistory = threads[tid].s0BwHistory;
    auto& s0LHistory = threads[tid].s0LHistory;

    const auto ghist_update = pred.getGHistUpdate();
    const auto bwhist_update = pred.getBwHistUpdate();
    const auto phist_update = pred.getPHistUpdate();

    // RAS updates its speculative stack, not folded history.
    if (ras->isEnabled()) {
        ras->specUpdateState(pred);
    }

    // Update component-local folded histories.
    for (int i = 0; i < numComponents; i++) {
        // use old histories to update predictor-local folded histories
        components[i]->specUpdateGHist(s0History, pred, ghist_update);
        components[i]->specUpdatePHist(s0PHistory, pred, phist_update);
    }
    if (mgsc->isEnabled()) {
        mgsc->specUpdateBwHist(s0BwHistory, pred, bwhist_update);
        mgsc->specUpdateIHist(pred, bwhist_update);
        mgsc->specUpdateLHist(s0LHistory, pred, ghist_update);
    }

    // Update global history
    histShiftIn(ghist_update.shamt, ghist_update.taken, s0History);

    // Update history manager and verify TAGE folded history
    historyManagers[tid].addSpeculativeHist(
        entry.startPC, entry.history, entry.phistory, ghist_update,
        phist_update, entry.predBranchInfo, fetch_id);

    // Update global backward history
    histShiftIn(bwhist_update.shamt, bwhist_update.taken, s0BwHistory);

    // Update path history
    pHistShiftIn(phist_update.shamt, phist_update.taken, s0PHistory,
                 phist_update.pc, phist_update.target);

    // Update local history
    const Addr localHistoryIndex =
        mgsc->getPcIndex(pred.bbStart,
                         log2(mgsc->getNumEntriesFirstLocalHistories()),
                         pred.asidHash);
    histShiftIn(ghist_update.shamt, ghist_update.taken,
        s0LHistory[localHistoryIndex]);

#ifndef NDEBUG
    if (tage->isEnabled()) {
        tage->checkFoldedHist(
            tage->usesPathHistory() ? s0PHistory : s0History, tid,
            "speculative update");
    }
    if (ittage->isEnabled()) {
        ittage->checkFoldedHist(s0PHistory, tid, "speculative update");
    }
    if (microtage->isEnabled()) {
        microtage->checkFoldedHist(s0PHistory, tid, "speculative update");
    }
    if (mgsc->isEnabled()) {
        mgsc->checkFoldedHist(s0History, s0PHistory, s0LHistory, tid,
                              "speculative update");
    }
#endif
}

/**
 * @brief Recovers branch history during a squash event
 *
 * @param target The target being squashed
 * @param target_id ID of the target being squashed
 * @param squash_pc PC where the squash occurred
 * @param is_conditional Whether the branch is conditional
 * @param actually_taken Whether the branch was actually taken
 * @param squash_type Type of squash (CTRL/OTHER/TRAP)
 */
void
DecoupledBPUWithBTB::recoverHistoryForSquash(
    FetchTarget &target,
    unsigned target_id,
    const PCStateBase &squash_pc,
    bool is_conditional,
    bool actually_taken,
    SquashType squash_type,
    Addr redirect_pc)
{
    ThreadID tid = target.tid;
    auto& s0History = threads[tid].s0History;
    auto& s0PHistory = threads[tid].s0PHistory;
    auto& s0BwHistory = threads[tid].s0BwHistory;
    auto& s0LHistory = threads[tid].s0LHistory;

    //printf("recover target_id: %u\n", target_id);
    // Restore history from the target
    s0History = target.history;
    s0PHistory = target.phistory;
    s0BwHistory = target.bwhistory;
    s0LHistory = target.lhistory;

    // Get actual history update information.
    const auto ghist_update = target.getGHistUpdateDuringSquash(
        squash_pc.instAddr(), is_conditional, actually_taken);
    const auto bwhist_update = target.getBwHistUpdateDuringSquash(
        squash_pc.instAddr(), is_conditional, actually_taken, redirect_pc);
    const auto phist_update = target.getPHistUpdateDuringSquash(
        squash_pc.instAddr(), actually_taken, redirect_pc);

    // RAS recovers its speculative stack, not folded history.
    if (ras->isEnabled()) {
        ras->recoverState(target);
    }
    if (abtb->isEnabled()) {
        abtb->recoverState(target);
    }

    // Recover component-local folded histories.
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, target, ghist_update.shamt,
                                   ghist_update.taken);
        components[i]->recoverPHist(s0PHistory, target, phist_update);
    }
    if (mgsc->isEnabled()) {
        mgsc->recoverBwHist(s0BwHistory, target, bwhist_update.shamt,
                            bwhist_update.taken);
        mgsc->recoverIHist(target, bwhist_update.shamt,
                           bwhist_update.taken);
        mgsc->recoverLHist(s0LHistory, target, ghist_update.shamt,
                           ghist_update.taken);
    }

    // Update global history with actual outcome
    histShiftIn(ghist_update.shamt, ghist_update.taken, s0History);

    // Update path history with actual outcome
    pHistShiftIn(phist_update.shamt, phist_update.taken, s0PHistory,
                 phist_update.pc, phist_update.target);

    // Update global backward history with actual outcome
    histShiftIn(bwhist_update.shamt, bwhist_update.taken, s0BwHistory);

    // Update local history with actual outcome
    const Addr localHistoryIndex =
        mgsc->getPcIndex(target.startPC,
                         log2(mgsc->getNumEntriesFirstLocalHistories()),
                         target.asidHash);
    histShiftIn(ghist_update.shamt, ghist_update.taken,
                s0LHistory[localHistoryIndex]);

    // Update history manager with appropriate branch info
    if (squash_type == SQUASH_CTRL) {
        historyManagers[tid].squash(target_id, ghist_update,
                                    phist_update,
                                    target.exeBranchInfo);
    } else {
        historyManagers[tid].squash(target_id, ghist_update,
                                    phist_update, BranchInfo());
    }

    // Perform history consistency checks when not a fast build variant
#ifndef NDEBUG
    checkHistories(s0History, s0PHistory, tid);
    if (tage->isEnabled()) {
        tage->checkFoldedHist(
            tage->usesPathHistory() ? s0PHistory : s0History, tid,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
    if (ittage->isEnabled()) {
        ittage->checkFoldedHist(s0PHistory, tid,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
    if (microtage->isEnabled()) {
        microtage->checkFoldedHist(s0PHistory, tid,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
    if (mgsc->isEnabled()) {
        mgsc->checkFoldedHist(s0History, s0PHistory, s0LHistory, tid,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
#endif
}


}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
