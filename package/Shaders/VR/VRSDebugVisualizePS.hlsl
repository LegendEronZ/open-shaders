// Multiplies the final framebuffer color by the VRS shading rate in effect at
// each pixel (green=1x1, yellow=1x2, orange=2x2, red=4x4), for visually
// verifying the actual per-eye region against the settings-UI diagram. Drawn
// as a full-screen triangle with a DEST_COLOR*SRC_COLOR blend state so it
// works against the swap-chain-backed framebuffer RTV (no UAV required).

cbuffer VRSDebugVisualizeCB : register(b0)
{
	uint TileWidth;
	uint TileHeight;
	uint OutputWidth;
	uint OutputHeight;
};

Texture2D<uint> RateTex : register(t0);

struct VS_OUTPUT
{
	float4 Position: SV_POSITION;
	float2 TexCoord: TEXCOORD0;
};

VS_OUTPUT VS_Main(uint vertexID : SV_VertexID)
{
	VS_OUTPUT output;
	output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(output.TexCoord * 2.0f - 1.0f, 0.0f, 1.0f);
	output.Position.y = -output.Position.y;
	return output;
}

float4 PS_Main(VS_OUTPUT input) :
	SV_TARGET
{
	uint2 pixelCoord = uint2(input.TexCoord * float2(OutputWidth, OutputHeight));
	uint2 tileCoord = uint2(
		min(pixelCoord.x * TileWidth / OutputWidth, TileWidth - 1),
		min(pixelCoord.y * TileHeight / OutputHeight, TileHeight - 1));
	uint rate = RateTex.Load(int3(tileCoord, 0));

	float3 tint;
	if (rate == 0)
		tint = float3(0.2, 1.0, 0.2);
	else if (rate == 1)
		tint = float3(1.0, 1.0, 0.2);
	else if (rate == 2)
		tint = float3(1.0, 0.6, 0.1);
	else
		tint = float3(1.0, 0.2, 0.2);

	// Blended via DEST_COLOR*SRC_COLOR (multiply): lerp toward the tint by
	// outputting lerp(1, tint, 0.5) so the destination is multiplied by that.
	return float4(lerp(float3(1, 1, 1), tint, 0.5), 1.0);
}
