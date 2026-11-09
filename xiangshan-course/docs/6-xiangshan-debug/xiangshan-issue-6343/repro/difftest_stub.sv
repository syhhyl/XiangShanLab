// Standalone elaboration shell for the non-functional difftest event sink.
// AtomicsUnit's functional RTL and interfaces are not replaced or modified.
module DummyDPICWrapper_LrScEvent (
  input clock,
  input reset,
  input io_valid,
  input io_bits_valid,
  input io_bits_success,
  input [7:0] io_bits_coreid
);
endmodule
