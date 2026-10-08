#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef PUSU_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif
#include "material_eval.glsl"
#ifdef PUSU_RAY_QUERY
#include "ray_query.glsl"
#endif
layout(location=0) in vec3 coordinates;
layout(location=1) in vec4 vertex_color;
layout(location=2) in vec3 world_position;
layout(location=3) in vec3 world_normal;
layout(location=4) in vec2 base_uv;
layout(location=5) in vec2 lm_uv;
layout(location=6) in float fog_depth;
layout(location=0) out vec4 output_color;
void main() {
    PusuSurface s;s.position=world_position;s.normal=world_normal;s.uv=base_uv;s.light_uv=lm_uv;s.color=vertex_color;s.triangle_id=0xffffffffu;s.barycentrics=vec2(0);
#ifdef PUSU_RAY_QUERY
    pusu_ray_receiver_footprint=max(length(dFdx(s.position)),length(dFdy(s.position)));
#endif
    GpuPass p=passes[draw.pass];vec4 raw=pusu_sample_coordinates(draw.pass,coordinates,-1);
#ifdef PUSU_RAY_QUERY
    if((p.modes.z&8u)!=0u&&frame.flags.z!=0u)raw.rgb=pusu_ray_reflection(draw.instance,draw.pass,s,s.position-frame.camera_time.xyz,raw.rgb,0);
#endif
    vec4 c=pusu_modulate_pass(draw.instance,draw.pass,s,raw);
    if(!pusu_pass_accepts_alpha(draw.pass,c.a))discard;
    vec3 legacy_albedo_proxy=c.rgb;
    c=pusu_fog_pass(draw.pass,c,fog_depth);
    if(frame.flags.x!=0u)c.rgb=pusu_decode(c.rgb);
#ifdef PUSU_RAY_QUERY
    if((p.modes.z&16u)!=0u&&frame.flags.w!=0u) {
        vec3 radiance=pusu_enhanced_directional(draw.instance,s,legacy_albedo_proxy);
        if(frame.flags.x!=0u)c.rgb+=radiance;
        else if(any(greaterThan(radiance,vec3(0))))c.rgb=pusu_encode(pusu_decode(c.rgb)+radiance);
    }
#endif
    output_color=c;
}
