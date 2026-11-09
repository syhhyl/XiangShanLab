#include <verilated.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <functional>
#ifdef FIXED
#include "VAtomicsUnitFixed.h"
using Dut = VAtomicsUnitFixed;
static constexpr bool kExpectStale = false;
static constexpr const char* kVariant = "fixed";
#else
#include "VAtomicsUnitOrig.h"
using Dut = VAtomicsUnitOrig;
static constexpr bool kExpectStale = true;
static constexpr const char* kVariant = "original";
#endif

static Dut* dut;
static uint64_t cycle_no;
static unsigned dcache_req_count;
static constexpr uint16_t AMOSWAP_D = 0x0b; // LSUOpType.amoswap_d
static constexpr unsigned TIMEOUT = 200;

static void clear_inputs() {
  dut->clock = 0; dut->reset = 0; dut->io_hartId = 0;
  dut->io_in_valid = 0; dut->io_in_bits_uop_fuOpType = 0;
  dut->io_in_bits_uop_rfWen = 0; dut->io_in_bits_uop_pdest = 0;
  dut->io_in_bits_uop_robIdx_flag = 0; dut->io_in_bits_uop_robIdx_value = 0;
  dut->io_in_bits_uop_sqIdx_flag = 0; dut->io_in_bits_uop_sqIdx_value = 0;
  dut->io_in_bits_src_0 = 0;
  dut->io_storeDataIn_0_valid = 0; dut->io_storeDataIn_0_bits_uop_fuOpType = 0;
  dut->io_storeDataIn_0_bits_data = 0; dut->io_storeDataIn_1_valid = 0;
  dut->io_storeDataIn_1_bits_uop_fuOpType = 0; dut->io_storeDataIn_1_bits_data = 0;
  // This generated standalone DUT has io.out.ready constant-propagated to 1.
  dut->io_dcache_req_ready = 1;
  dut->io_dcache_resp_valid = 0; dut->io_dcache_resp_bits_data[0] = 0;
  dut->io_dcache_resp_bits_data[1] = 0; dut->io_dcache_resp_bits_data[2] = 0;
  dut->io_dcache_resp_bits_data[3] = 0; dut->io_dcache_resp_bits_miss = 0;
  dut->io_dcache_resp_bits_replay = 0;
  dut->io_dcache_resp_bits_tl_error_tl_denied = 0;
  dut->io_dcache_resp_bits_tl_error_tl_corrupt = 0;
  dut->io_dcache_resp_bits_id = 0; dut->io_dcache_block_lr = 0;
  dut->io_dtlb_resp_valid = 0; dut->io_dtlb_resp_bits_paddr_0 = 0;
  dut->io_dtlb_resp_bits_gpaddr_0 = 0; dut->io_dtlb_resp_bits_fullva = 0;
  dut->io_dtlb_resp_bits_pbmt_0 = 0; dut->io_dtlb_resp_bits_miss = 0;
  dut->io_dtlb_resp_bits_isForVSnonLeafPTE = 0;
  dut->io_dtlb_resp_bits_excp_0_gpf_ld = 0; dut->io_dtlb_resp_bits_excp_0_gpf_st = 0;
  dut->io_dtlb_resp_bits_excp_0_pf_ld = 0; dut->io_dtlb_resp_bits_excp_0_pf_st = 0;
  dut->io_dtlb_resp_bits_excp_0_af_ld = 0; dut->io_dtlb_resp_bits_excp_0_af_st = 0;
  dut->io_pmpResp_ld = 0; dut->io_pmpResp_st = 0; dut->io_pmpResp_mmio = 0;
  dut->io_flush_sbuffer_empty = 1; dut->io_redirect_valid = 0;
  dut->io_csrCtrl_cache_error_enable = 1;
  dut->io_csrCtrl_mem_trigger_tUpdate_valid = 0;
  dut->io_csrCtrl_mem_trigger_tUpdate_bits_addr = 0;
  dut->io_csrCtrl_mem_trigger_tUpdate_bits_tdata_matchType = 0;
  dut->io_csrCtrl_mem_trigger_tUpdate_bits_tdata_select = 0;
  dut->io_csrCtrl_mem_trigger_tUpdate_bits_tdata_action = 0;
  dut->io_csrCtrl_mem_trigger_tUpdate_bits_tdata_chain = 0;
  dut->io_csrCtrl_mem_trigger_tUpdate_bits_tdata_store = 0;
  dut->io_csrCtrl_mem_trigger_tUpdate_bits_tdata_load = 0;
  dut->io_csrCtrl_mem_trigger_tUpdate_bits_tdata_tdata2 = 0;
  dut->io_csrCtrl_mem_trigger_tEnableVec_0 = 0; dut->io_csrCtrl_mem_trigger_tEnableVec_1 = 0;
  dut->io_csrCtrl_mem_trigger_tEnableVec_2 = 0; dut->io_csrCtrl_mem_trigger_tEnableVec_3 = 0;
  dut->io_csrCtrl_mem_trigger_debugMode = 0;
  dut->io_csrCtrl_mem_trigger_triggerCanRaiseBpExp = 0;
}

static void tick() {
  dut->clock = 0; dut->eval();
  dut->clock = 1; dut->eval();
  ++cycle_no;
}
static void fail(const std::string& why) {
  std::cerr << "FAIL[" << kVariant << "] cycle=" << cycle_no << ": " << why
            << " in.ready=" << int(dut->io_in_ready)
            << " tlb.req=" << int(dut->io_dtlb_req_valid)
            << " dc.req=" << int(dut->io_dcache_req_valid)
            << " out.valid=" << int(dut->io_out_valid) << "\n";
  throw std::runtime_error(why);
}
static void wait_signal(const char* name, const std::function<bool()>& pred) {
  for (unsigned i = 0; i < TIMEOUT; ++i) { dut->eval(); if (pred()) return; tick(); }
  fail(std::string("timeout waiting for ") + name);
}
static void issue_sta(uint64_t addr, uint8_t pdest) {
  wait_signal("io_in_ready", [] { return dut->io_in_ready; });
  dut->io_in_bits_uop_fuOpType = AMOSWAP_D;
  dut->io_in_bits_uop_rfWen = 1; dut->io_in_bits_uop_pdest = pdest;
  dut->io_in_bits_src_0 = addr; dut->io_in_valid = 1; dut->eval();
  std::cout << "cycle " << cycle_no << " STA fire addr=0x" << std::hex << addr
            << " fuOpType=0x" << AMOSWAP_D << std::dec << "\n";
  tick(); dut->io_in_valid = 0;
}
static void issue_std(uint64_t data) {
  dut->io_storeDataIn_0_bits_uop_fuOpType = AMOSWAP_D;
  dut->io_storeDataIn_0_bits_data = data; dut->io_storeDataIn_0_valid = 1;
  dut->eval(); std::cout << "cycle " << cycle_no << " STD fire data=0x"
                         << std::hex << data << std::dec << "\n";
  tick(); dut->io_storeDataIn_0_valid = 0;
}
static void send_tlb_hit(uint64_t addr) {
  // Deliberately answer the first request: AtomicsUnit must ignore it.
  wait_signal("first dtlb req", [] { return dut->io_dtlb_req_valid; });
  dut->io_dtlb_resp_bits_paddr_0 = addr & ((1ULL << 48) - 1);
  dut->io_dtlb_resp_bits_fullva = addr; dut->io_dtlb_resp_bits_gpaddr_0 = addr;
  dut->io_dtlb_resp_valid = 1; dut->eval();
  std::cout << "cycle " << cycle_no << " TLB resp #1 (expected ignored) addr=0x"
            << std::hex << addr << std::dec << "\n";
  tick();
  if (!dut->io_dtlb_req_valid) fail("first TLB response was not ignored");
  dut->eval();
  std::cout << "cycle " << cycle_no << " TLB resp #2 accepted addr=0x"
            << std::hex << addr << std::dec << "\n";
  tick(); dut->io_dtlb_resp_valid = 0;
}
static void wait_cache_req() {
  wait_signal("dcache request", [] { return dut->io_dcache_req_valid; });
  dut->eval(); ++dcache_req_count;
  std::cout << "cycle " << cycle_no << " DCACHE req fire cmd=0x" << std::hex
            << unsigned(dut->io_dcache_req_bits_cmd) << " addr=0x"
            << uint64_t(dut->io_dcache_req_bits_addr) << std::dec << "\n";
  tick();
}
static void send_corrupt_resp() {
  dut->io_dcache_resp_valid = 1; dut->io_dcache_resp_bits_miss = 0;
  dut->io_dcache_resp_bits_replay = 0; dut->io_dcache_resp_bits_id = 1;
  dut->io_dcache_resp_bits_tl_error_tl_denied = 0;
  dut->io_dcache_resp_bits_tl_error_tl_corrupt = 1;
  dut->io_dcache_resp_bits_data[0] = 0x55667788; dut->io_dcache_resp_bits_data[1] = 0x11223344;
  dut->eval(); std::cout << "cycle " << cycle_no << " DCACHE resp fire miss=0 replay=0 corrupt=1 denied=0\n";
  tick(); dut->io_dcache_resp_valid = 0; dut->io_dcache_resp_bits_tl_error_tl_corrupt = 0;
}
struct Wb { bool b6; bool b19; };
static Wb consume_wb(const char* op) {
  wait_signal("writeback", [] { return dut->io_out_valid; });
  dut->eval(); Wb w{bool(dut->io_out_bits_uop_exceptionVec_6), bool(dut->io_out_bits_uop_exceptionVec_19)};
  std::cout << "cycle " << cycle_no << " " << op << " WB fire bit6=" << w.b6
            << " bit19=" << w.b19 << "\n";
  tick(); // out.ready=1: this is a real io.out.fire and resetFSM completion
  return w;
}
int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv); dut = new Dut; clear_inputs();
  try {
    dut->reset = 1; for (int i = 0; i < 4; ++i) tick();
    dut->reset = 0; tick();
    std::cout << "variant=" << kVariant << " reset complete\n";
    issue_sta(0x1000, 1); send_tlb_hit(0x1000);
    issue_std(0xdeadbeefcafebabeULL); wait_cache_req(); send_corrupt_resp();
    Wb a = consume_wb("opA");
    if (a.b6 || !a.b19) fail("op A expected bit6=0 bit19=1");
    wait_signal("idle after opA", [] { return dut->io_in_ready; });
    issue_sta(0x2003, 2); send_tlb_hit(0x2003);
    unsigned req_before = dcache_req_count;
    Wb b = consume_wb("opB");
    if (dcache_req_count != req_before || dut->io_dcache_req_valid)
      fail("misaligned op B unexpectedly reached dcache");
    if (!b.b6) fail("op B missing storeAddrMisaligned bit6");
    if (b.b19 != kExpectStale)
      fail(std::string("op B bit19 mismatch, expected ") + (kExpectStale ? "1" : "0"));
    std::cout << "PASS[" << kVariant << "]: opA bit19=1; opB bit6=1 bit19=" << b.b19
              << "; opB dcache requests=0\n";
    delete dut; return 0;
  } catch (const std::exception&) { delete dut; return 1; }
}
