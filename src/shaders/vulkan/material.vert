#version 460
#extension GL_GOOGLE_include_directive : require
#include "material_eval.glsl"
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec2 light_uv;
layout(location=3) in vec3 normal;
layout(location=4) in vec4 color;
layout(location=0) out vec3 coordinates;
layout(location=1) out vec4 vertex_color;
layout(location=2) out vec3 world_position;
layout(location=3) out vec3 world_normal;
layout(location=4) out vec2 base_uv;
layout(location=5) out vec2 lm_uv;
layout(location=6) out float fog_depth;
void main() {
    PusuSurface s;s.position=position;s.normal=normal;s.uv=uv;s.light_uv=light_uv;s.color=color;s.triangle_id=0xffffffffu;s.barycentrics=vec2(0);
    s=pusu_world_surface(draw.instance,s);
    bool ui=(draw.flags&1u)!=0u;
    coordinates=pusu_coordinates(draw.pass,s,ui?vec3(0):frame.camera_time.xyz,vec3(0));
    vertex_color=color;world_position=s.position;world_normal=s.normal;base_uv=uv;lm_uv=light_uv;
    fog_depth=abs((frame.view*vec4(s.position,1)).z);
    vec4 clip;
    if(ui){vec2 size=(draw.flags&2u)!=0u?vec2(float(draw.flags>>2),uintBitsToFloat(draw.history_layer)):vec2(1024,768);clip=vec4(s.position.x*2/size.x-1,1-s.position.y*2/size.y,s.position.z,1);}
    else clip=frame.view_projection*vec4(s.position,1);
    clip.z=(clip.z+clip.w)*0.5;gl_Position=clip;
}
