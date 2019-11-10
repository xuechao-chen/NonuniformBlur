//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "MipGaussian.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
Texture2D<float3>	g_txCoarser;

//--------------------------------------------------------------------------------------
// Pixel shader
//--------------------------------------------------------------------------------------
float4 main(PSIn input) : SV_TARGET
{
	// Fetch the color of the resolved color at the coarser level
	const float3 coarser = g_txCoarser.SampleLevel(g_smpLinear, input.Tex, 0.0);

	// Gaussian-approximating Haar coefficients (weights of box filters)
	const float weight = MipGaussianBlendWeight(input.Tex);

	return float4(coarser, 1.0 - weight);
}
