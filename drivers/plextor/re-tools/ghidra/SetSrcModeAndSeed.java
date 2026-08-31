// Set the MCS-251 srcMode context bit and seed disassembly from ECALL targets.
// Ghidra's 8051 module implements source mode (8051_main.sinc: srcMode = UCONFIG0.0)
// but defaults it to 0 (binary mode), which decodes this firmware as noise.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import java.io.*;
import java.math.BigInteger;

public class SetSrcModeAndSeed extends GhidraScript {
    @Override
    public void run() throws Exception {
        Register srcMode = currentProgram.getProgramContext().getRegister("srcMode");
        if (srcMode == null) {
            println("FATAL: processor has no srcMode register");
            return;
        }
        AddressSetView init = currentProgram.getMemory().getLoadedAndInitializedAddressSet();
        for (AddressRange r : init) {
            currentProgram.getProgramContext().setValue(
                srcMode, r.getMinAddress(), r.getMaxAddress(), BigInteger.ONE);
        }
        println("SRCMODE: set to 1 over " + init);

        File f = new File("/var/tmp/seeds.txt");
        int n = 0, ok = 0;
        if (f.exists()) {
            BufferedReader br = new BufferedReader(new FileReader(f));
            String line;
            while ((line = br.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                Address ad = toAddr(Long.decode(line).longValue());
                n++;
                try {
                    if (disassemble(ad)) { createFunction(ad, null); ok++; }
                } catch (Exception e) { /* seed may be data; skip */ }
                if (monitor.isCancelled()) break;
            }
            br.close();
        }
        println("SEEDS: " + ok + "/" + n + " disassembled and made functions");
    }
}
