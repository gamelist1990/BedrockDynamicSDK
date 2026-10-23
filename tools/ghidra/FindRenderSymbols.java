// Ghidra headless script: list all known symbols and function ranges related
// to LevelRendererCamera/ViewRenderObject from the selected exact-build PE.
// Run via analyzeHeadless ... -postScript FindRenderSymbols.java
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;

public class FindRenderSymbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        long[] candidates = { 0x14EF9C19AL, 0x14EC139A4L };
        for (long value : candidates) {
            Address address = toAddr(value);
            println("references to string/data " + address);
            for (Reference reference : getReferencesTo(address)) {
                var function = getFunctionContaining(reference.getFromAddress());
                println("  " + reference.getFromAddress() + " " + reference.getReferenceType() +
                        " function=" + (function == null ? "<none>" : function.getEntryPoint()));
            }
        }
    }
}
