// Post-analysis report for the PX-716A image: coverage, hot functions, and the
// containing function + inbound references for the addresses of interest.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class Report251 extends GhidraScript {
    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing l = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();

        long insn = 0, cov = 0;
        InstructionIterator it = l.getInstructions(true);
        while (it.hasNext()) { Instruction i = it.next(); insn++; cov += i.getLength(); }
        AddressSetView init = currentProgram.getMemory().getLoadedAndInitializedAddressSet();
        long total = init.getNumAddresses();
        int nf = fm.getFunctionCount();

        StringBuilder sb = new StringBuilder();
        sb.append(String.format("functions=%d instructions=%d codeBytes=%d totalBytes=%d%n",
                                nf, insn, cov, total));
        sb.append(String.format("code coverage = %.1f%% of image%n", total > 0 ? 100.0 * cov / total : 0.0));

        long[] poi = { 0xf65b83L, 0xfbffa8L, 0xfadce4L, 0xfad888L, 0xf00050L };
        String[] lbl = { "read-speed ladder table", "dispatcher chain (no-media)",
                         "region-code check byte", "region-check function", "vector table" };
        sb.append("\n-- addresses of interest --\n");
        for (int i = 0; i < poi.length; i++) {
            Address a = toAddr(poi[i]);
            Function f = fm.getFunctionContaining(a);
            CodeUnit cu = l.getCodeUnitContaining(a);
            sb.append(String.format("  %-28s %s  func=%s  unit=%s  refsTo=%d%n",
                lbl[i], a,
                f == null ? "(none)" : f.getEntryPoint().toString(),
                cu == null ? "(none)" : cu.getMnemonicString(),
                rm.getReferenceCountTo(a)));
        }

        List<Function> fs = new ArrayList<>();
        for (Function f : fm.getFunctions(true)) fs.add(f);
        fs.sort((a, b) -> Integer.compare(rm.getReferenceCountTo(b.getEntryPoint()),
                                          rm.getReferenceCountTo(a.getEntryPoint())));
        sb.append("\n-- top 25 functions by inbound references --\n");
        for (int i = 0; i < Math.min(25, fs.size()); i++) {
            Function f = fs.get(i);
            sb.append(String.format("  %s refs=%-4d size=%d%n", f.getEntryPoint(),
                rm.getReferenceCountTo(f.getEntryPoint()), f.getBody().getNumAddresses()));
        }
        println(sb.toString());
        PrintWriter pw = new PrintWriter("/var/tmp/ghidra_out.txt");
        pw.print(sb); pw.close();
        println("wrote /var/tmp/ghidra_out.txt");
    }
}
