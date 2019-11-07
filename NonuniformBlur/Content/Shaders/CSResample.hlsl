//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "CSResample.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_txDest;

//--------------------------------------------------------------------------------------
// Compute shader
//--------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float2 dim;
	g_txDest.GetDimensions(dim.x, dim.y);

	const float2 tex = (DTid + 0.5) / dim;

	g_txDest[DTid] = Resample(tex);
}
