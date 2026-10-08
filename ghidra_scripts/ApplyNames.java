// Applies the names carried over from the linker map (names_v103.csv) to the program:
// creates any function Ghidra missed, then demangles the MSVC name onto it.
// Usage (headless): -postScript ApplyNames.java <names_v103.csv>
//@category Imperialism2

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.HashSet;
import java.util.Set;

import ghidra.app.cmd.label.DemanglerCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyNames extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File in = args.length > 0 ? new File(args[0]) : askFile("names_v103.csv", "Open");

		Set<Long> done = new HashSet<>();
		int named = 0, created = 0, demangled = 0, skipped = 0;

		try (BufferedReader r = new BufferedReader(new FileReader(in))) {
			r.readLine(); // header: addr,map_addr,shift,hit,obj,name
			String line;
			while ((line = r.readLine()) != null && !monitor.isCancelled()) {
				String[] p = line.split(",", 6);
				if (p.length < 6) {
					continue;
				}
				// Library code is left to Ghidra's own signatures; unconfirmed starts are skipped.
				if (p[4].contains(":") || !p[3].equals("1")) {
					skipped++;
					continue;
				}
				long offset = Long.parseLong(p[0], 16);
				if (!done.add(offset)) {
					continue; // several names share this address; the first one wins
				}
				Address addr = toAddr(offset);
				String mangled = p[5];

				Function f = getFunctionAt(addr);
				if (f == null) {
					disassemble(addr);
					f = createFunction(addr, null);
					if (f == null) {
						skipped++;
						continue;
					}
					created++;
				}

				DemanglerCmd cmd = new DemanglerCmd(addr, mangled);
				if (cmd.applyTo(currentProgram, monitor)) {
					demangled++;
				}
				else {
					f.setName(mangled.replaceAll("[^A-Za-z0-9_@?$]", "_"), SourceType.IMPORTED);
				}
				f.setComment("obj: " + p[4] + "\nmangled: " + mangled);
				named++;
			}
		}
		println("Named " + named + " functions (" + demangled + " demangled, " + created +
			" newly created, " + skipped + " rows skipped)");
	}
}
