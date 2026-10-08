// Dumps every function Ghidra found (address, body size, name, call counts) to a CSV.
// Usage (headless): -postScript ExportFunctions.java <output.csv>
//@category Imperialism2

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportFunctions extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File out = args.length > 0 ? new File(args[0]) : askFile("Output CSV", "Save");

		int count = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("addr,size,is_thunk,callers,callees,name");
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				w.printf("%08x,%d,%d,%d,%d,%s%n",
					f.getEntryPoint().getOffset(),
					f.getBody().getNumAddresses(),
					f.isThunk() ? 1 : 0,
					f.getCallingFunctions(monitor).size(),
					f.getCalledFunctions(monitor).size(),
					f.getName(true));
				count++;
			}
		}
		println("Exported " + count + " functions to " + out);
	}
}
