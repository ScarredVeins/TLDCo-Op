// Fallback for TLD's "Shader Forge/TLD_StandardSkinned_2Sided" (eyelashes, hair cards): alpha-tested, no culling.
Shader "TLDCoop/Body2Sided" {
    Properties {
        _Color ("Main colour", Color) = (1,1,1,1)
        _MainTex ("MainTex", 2D) = "white" {}
        _Cutoff ("Alpha cutoff", Range(0, 1)) = 0.5
    }
    SubShader {
        Tags { "Queue" = "AlphaTest" "RenderType" = "TransparentCutout" }
        Cull Off
        CGPROGRAM
        #pragma surface surf Standard alphatest:_Cutoff addshadow
        #pragma target 3.0
        sampler2D _MainTex;
        fixed4 _Color;
        struct Input { float2 uv_MainTex; };
        void surf(Input IN, inout SurfaceOutputStandard o) {
            fixed4 c = tex2D(_MainTex, IN.uv_MainTex) * _Color;
            o.Albedo = c.rgb;
            o.Alpha = c.a;
            o.Smoothness = 0.1;
        }
        ENDCG
    }
    FallBack "Legacy Shaders/Transparent/Cutout/VertexLit"
}
