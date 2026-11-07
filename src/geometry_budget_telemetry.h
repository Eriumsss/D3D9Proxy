#pragma once
// Read-only Direct3D9 static geometry creation telemetry, OFF by default.
// This tracks cumulative *successful allocations*, NOT live GPU residency.
// No COM buffer wrapping, pool capacity patching, or mission loading.
#include <d3d9.h>
void OC_GeometryBudget_Init();
void OC_GeometryBudget_VB(UINT bytes, DWORD usage, D3DPOOL pool,
                         HRESULT result, bool outputValid, void* caller);
void OC_GeometryBudget_IB(UINT bytes, DWORD usage, D3DPOOL pool,
                         HRESULT result, bool outputValid, void* caller);
