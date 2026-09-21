cbuffer SceneConstants : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 localToWorld;
};

struct SceneVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct ScenePixel
{
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

ScenePixel vs_main(SceneVertex input)
{
    ScenePixel output;
    const float4 worldPosition = mul(float4(input.position, 1.0F), localToWorld);
    output.position = mul(worldPosition, viewProjection);
    output.color = abs(input.normal) * 0.75F + 0.25F;
    return output;
}

float4 ps_main(ScenePixel input) : SV_TARGET
{
    return float4(input.color, 1.0F);
}
