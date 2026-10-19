// Fallback for TLD's "Shader Forge/TLD_StandardSkinned (Characters)". The mod swaps in TLD's own skinned shader
// at runtime when one is loaded; property names match so the textures carry over.
Shader "TLDCoop/Body" {
    Properties {
        _Color ("Main colour", Color) = (1,1,1,1)
        _MainTex ("MainTex", 2D) = "white" {}
        _Normal ("Normal", 2D) = "bump" {}
        _NormalMapStrength ("Normal Map Strength", Range(0, 2)) = 1
    }
    SubShader {
        Tags { "RenderType" = "Opaque" }
        LOD 200
        CGPROGRAM
        #pragma surface surf Standard fullforwardshadows
        #pragma target 3.0
        sampler2D _MainTex;
        sampler2D _Normal;
        fixed4 _Color;
        half _NormalMapStrength;
        struct Input { float2 uv_MainTex; };
        void surf(Input IN, inout SurfaceOutputStandard o) {
            fixed4 c = tex2D(_MainTex, IN.uv_MainTex) * _Color;
            o.Albedo = c.rgb;
            o.Normal = UnpackScaleNormal(tex2D(_Normal, IN.uv_MainTex), _NormalMapStrength);
            o.Smoothness = 0.15;
            o.Metallic = 0;
            o.Alpha = 1;
        }
        ENDCG
    }
    FallBack "Diffuse"
}
