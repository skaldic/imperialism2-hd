// Decompiles every game function named from the linker map and writes one pseudo-C file per
// original object file (UDisplayMgr.obj -> UDisplayMgr.c), functions in address order.
// Usage (headless): -postScript DecompileByObj.java <names_v103.csv> <output dir>
//@category Imperialism2

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileByObj extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File in = new File(args[0]);
		File outDir = new File(args[1]);
		outDir.mkdirs();

		// obj name -> function addresses, both sorted
		Map<String, TreeMap<Long, String>> byObj = new TreeMap<>();
		try (BufferedReader r = new BufferedReader(new FileReader(in))) {
			r.readLine();
			String line;
			while ((line = r.readLine()) != null) {
				String[] p = line.split(",", 6);
				if (p.length < 6 || p[4].contains(":") || !p[3].equals("1")) {
					continue;
				}
				byObj.computeIfAbsent(p[4], k -> new TreeMap<>())
					.putIfAbsent(Long.parseLong(p[0], 16), p[5]);
			}
		}

		DecompInterface decomp = new DecompInterface();
		decomp.openProgram(currentProgram);

		int ok = 0, failed = 0;
		List<String> failures = new ArrayList<>();
		for (Map.Entry<String, TreeMap<Long, String>> obj : byObj.entrySet()) {
			if (monitor.isCancelled()) {
				break;
			}
			String base = obj.getKey().replaceAll("\\.obj$", "");
			try (PrintWriter w = new PrintWriter(new File(outDir, base + ".c"), "UTF-8")) {
				w.println("// Decompiled from Imperialism II.exe (v1.03), original object: " + obj.getKey());
				w.println("// Ghidra pseudo-C: a reading aid, not compilable source.");
				for (Map.Entry<Long, String> fn : obj.getValue().entrySet()) {
					Function f = getFunctionAt(toAddr(fn.getKey()));
					if (f == null) {
						continue;
					}
					DecompileResults res = decomp.decompileFunction(f, 60, monitor);
					w.println();
					w.printf("// %08x  %s%n", fn.getKey(), fn.getValue());
					if (res != null && res.decompileCompleted()) {
						w.print(res.getDecompiledFunction().getC());
						ok++;
					}
					else {
						w.println("// (decompilation failed)");
						failures.add(f.getName(true));
						failed++;
					}
				}
			}
		}
		decomp.dispose();
		println("Decompiled " + ok + " functions into " + byObj.size() + " files (" + failed + " failed)");
	}
}
