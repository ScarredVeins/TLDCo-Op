// Fallback for TLD's transparent skinned shaders (glasses): alpha-blended, glossy.
Shader "TLDCoop/BodyTransparent" {
    Properties {
        _Color ("Main colour", Color) = (1,1,1,0.3)
        _MainTex ("MainTex", 2D) = "white" {}
    }
    SubShader {
        Tags { "Queue" = "Transparent" "RenderType" = "Transparent" }
        ZWrite Off
        CGPROGRAM
        #pragma surface surf Standard alpha:fade
        #pragma target 3.0
        sampler2D _MainTex;
        fixed4 _Color;
        struct Input { float2 uv_MainTex; };
        void surf(Input IN, inout SurfaceOutputStandard o) {
            fixed4 c = tex2D(_MainTex, IN.uv_MainTex) * _Color;
            o.Albedo = c.rgb;
            o.Alpha = c.a;
            o.Smoothness = 0.9;
        }
        ENDCG
    }
}
