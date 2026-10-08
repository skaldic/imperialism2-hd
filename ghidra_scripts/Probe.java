// Ad-hoc inspection helper for headless runs.
//   Probe.java <out file> range <start> <end>     decompile every function in [start, end)
//   Probe.java <out file> xrefs <addr> [addr...]  list references to each address, with the
//                                                 containing function and reference type
//@category Imperialism2

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class Probe extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		try (PrintWriter w = new PrintWriter(new File(args[0]), "UTF-8")) {
			if (args[1].equals("range")) {
				decompileRange(w, toAddr(Long.parseLong(args[2], 16)), toAddr(Long.parseLong(args[3], 16)));
			}
			else {
				for (int i = 2; i < args.length; i++) {
					xrefs(w, toAddr(Long.parseLong(args[i], 16)));
				}
			}
		}
	}

	private void decompileRange(PrintWriter w, Address start, Address end) throws Exception {
		DecompInterface decomp = new DecompInterface();
		decomp.openProgram(currentProgram);
		FunctionIterator it = currentProgram.getFunctionManager()
			.getFunctions(new AddressSet(start, end.subtract(1)), true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Function f = it.next();
			if (f.isThunk()) {
				continue;
			}
			DecompileResults res = decomp.decompileFunction(f, 60, monitor);
			w.printf("%n// %s  %s%n", f.getEntryPoint(), f.getName(true));
			if (res != null && res.decompileCompleted()) {
				w.print(res.getDecompiledFunction().getC());
			}
			else {
				w.println("// (decompilation failed)");
			}
		}
		decomp.dispose();
	}

	private void xrefs(PrintWriter w, Address target) {
		w.printf("%n== references to %s ==%n", target);
		ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(target);
		while (it.hasNext()) {
			Reference r = it.next();
			Function f = getFunctionContaining(r.getFromAddress());
			w.printf("%s  %-12s %s%n", r.getFromAddress(), r.getReferenceType(),
				f == null ? "(no function)" : f.getName(true) + " @ " + f.getEntryPoint());
		}
	}
}
