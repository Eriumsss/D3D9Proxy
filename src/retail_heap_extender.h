#pragma once
// The retail heap extender is a voluntary, opt-in integration of the verified
// NpcProxy heap-growth design (DLL/NpcProxy/PLAN.md, section 12.2).
// It never changes the retail EXE's .text and never replaces malloc/free.
// The original CRT arena remains owned by the game.
void OC_HeapExt_Init();
void OC_HeapExt_Tick();
