import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class Callers251 extends GhidraScript {
    @Override
    public void run() throws Exception {
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        long[] roots = { 0xf65b36L, 0xf65b83L };
        for (long r0 : roots) {
            println("######## callers of " + toAddr(r0) + " ########");
            Set<Address> seen = new HashSet<>();
            List<Address> cur = new ArrayList<>();
            cur.add(toAddr(r0)); seen.add(toAddr(r0));
            for (int depth = 1; depth <= 4 && !cur.isEmpty(); depth++) {
                List<Address> next = new ArrayList<>();
                for (Address a : cur) {
                    ReferenceIterator ri = rm.getReferencesTo(a);
                    while (ri.hasNext()) {
                        Reference rf = ri.next();
                        Address from = rf.getFromAddress();
                        Function f = fm.getFunctionContaining(from);
                        Address key = (f == null) ? from : f.getEntryPoint();
                        if (seen.add(key)) {
                            next.add(key);
                            println(String.format("  depth %d: %s  (site %s, %s)",
                                depth, key, from, rf.getReferenceType()));
                        }
                    }
                }
                cur = next;
                if (next.size() > 25) { println("  ... widening, stop"); break; }
            }
        }
    }
}
