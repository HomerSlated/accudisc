import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;

public class Listing251 extends GhidraScript {
    @Override
    public void run() throws Exception {
        long[] at = { 0xf7822eL, 0xf94e2eL, 0xfd3e1aL, 0xf65b36L };
        Listing l = currentProgram.getListing();
        for (long a0 : at) {
            Address a = toAddr(a0);
            println("=== " + a + " ===");
            if (l.getInstructionAt(a) == null) { disassemble(a); }
            Instruction ins = l.getInstructionAt(a);
            for (int k = 0; k < 22 && ins != null; k++) {
                StringBuilder b = new StringBuilder();
                try { for (byte x : ins.getBytes()) b.append(String.format("%02x", x)); }
                catch (Exception e) { b.append("??"); }
                println(String.format("  %s  %-12s %s", ins.getAddress(), b, ins.toString()));
                ins = ins.getNext();
            }
        }
    }
}
