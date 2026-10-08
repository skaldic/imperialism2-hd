// Lists every instruction whose disassembly text matches a regular expression, with its bytes
// and containing function. Used to enumerate patch sites.
// Usage (headless): -postScript FindInstructions.java <out file> <regex> [startAddr endAddr]
//@category Imperialism2

import java.io.File;
import java.io.PrintWriter;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class FindInstructions extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		Pattern pattern = Pattern.compile(args[1]);
		InstructionIterator it = args.length >= 4
			? currentProgram.getListing().getInstructions(
				new AddressSet(toAddr(Long.parseLong(args[2], 16)), toAddr(Long.parseLong(args[3], 16))), true)
			: currentProgram.getListing().getInstructions(true);

		int found = 0;
		try (PrintWriter w = new PrintWriter(new File(args[0]), "UTF-8")) {
			while (it.hasNext() && !monitor.isCancelled()) {
				Instruction ins = it.next();
				String text = ins.toString();
				if (!pattern.matcher(text).find()) {
					continue;
				}
				StringBuilder bytes = new StringBuilder();
				for (byte b : ins.getBytes()) {
					bytes.append(String.format("%02x ", b));
				}
				Function f = getFunctionContaining(ins.getAddress());
				w.printf("%s\t%s\t%s\t%s%n", ins.getAddress(), bytes.toString().trim(), text,
					f == null ? "(no function)" : f.getName(true) + "@" + f.getEntryPoint());
				found++;
			}
		}
		println("FindInstructions: " + found + " matches for " + args[1]);
	}
}
