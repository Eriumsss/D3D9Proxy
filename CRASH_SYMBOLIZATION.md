# Alt+Enter crash: symbols and isolated repro

The provided crash handler indicates read access violation 0xC0000005 inside d3d9.dll, base 0x6DBD0000, EIP 0x6DBDB1B9, so RVA 0xB1B9. The failing read address was 0x9FB90408. This alone cannot identify a definitive C++ function.

Source-level suspects include the TextureVault COM texture lifetime and the device Reset/overlay callbacks. These are hypotheses, not a proven root cause.

For an isolation test in a disposable game copy, retain registry expansion flags, but use:

    [RetailHeapExtender]
    Enabled=0
    [WorldResourceMemory]
    TextureVault=0
    Monitor=0

If the repeated Alt+Enter test becomes stable, enable TextureVault alone, and next HeapExtender alone to isolate an owner. If it still crashes, disable the native device reset and dynamic buffer reset expansion options for another comparison.

This PR makes each MSVC x86 release build produce d3d9.dll, d3d9.map and d3d9.pdb together. Use matching files from the SAME build. Do not map the old crash RVA against a different rebuild.

After a fresh crash with the new installed DLL:

    python tools/symbolize_proxy_crash.py --map d3d9.map --eip 0x6DBDB1B9 --module-base 0x6DBD0000

Replace the addresses with the NEW report. The script prints the nearest public symbols and explicitly warns that a nearest symbol is not exact proof. For precise call stacks, load d3d9.pdb matching the installed DLL in WinDbg:

    .sympath+ C:\Path\To\MatchingSymbols
    .reload /f d3d9.dll
    lmvm d3d9
    ln d3d9+0xB1B9
    .ecxr
    kv

A matching minidump plus PDB is needed to establish the actual instruction, stack trace and source line, not just an RVA. This PR provides traceability; it does not claim to fix the Alt+Enter crash.
