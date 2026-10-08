#ifndef PUSU_MATERIAL_EVAL
#define PUSU_MATERIAL_EVAL
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
struct GpuPass { mat4 tc_matrix; vec4 constant_color; uvec4 binding; uvec4 modes; uvec4 blend; vec4 parameters; };
struct GpuInstance { mat4 model; vec4 normal0; vec4 normal1; vec4 normal2; vec4 entity_color; uvec4 ranges; uvec4 metadata; };
struct GpuGeometry { uvec4 addresses; uvec4 counts; };
struct GpuTriangle { uvec4 vertices; uvec4 source; };
struct GpuCoverage { uvec4 passes; };
layout(set=0,binding=0,std140) uniform Frame { mat4 view; mat4 view_projection; vec4 camera_time; vec4 fog_color; vec4 fog_parameters; vec4 light_direction; vec4 light_color; vec4 quality; uvec4 flags; } frame;
layout(set=0,binding=1,std430) readonly buffer Passes { GpuPass passes[]; };
layout(set=0,binding=2,std430) readonly buffer Instances { GpuInstance instances[]; };
layout(set=0,binding=3,std430) readonly buffer Geometries { GpuGeometry geometries[]; };
layout(set=0,binding=4,std430) readonly buffer Triangles { GpuTriangle triangles[]; };
layout(set=0,binding=5) uniform texture2D images[];
layout(set=0,binding=6) uniform textureCube cubes[];
layout(set=0,binding=7) uniform sampler samplers[19];
layout(set=0,binding=8,std430) readonly buffer Coverages { GpuCoverage coverages[]; };
layout(set=0,binding=9,std430) readonly buffer CoveragePasses { uint coverage_passes[]; };
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer VertexWords { float words[]; };
layout(push_constant) uniform Draw { uint instance; uint pass; uint flags; uint history_layer; } draw;
struct PusuSurface { vec3 position; vec3 normal; vec2 uv; vec2 light_uv; vec4 color; uint triangle_id; vec2 barycentrics; };
bool pusu_finite(vec3 v) { return !any(isnan(v))&&!any(isinf(v)); }
PusuSurface pusu_vertex(uint geometry_id,uint vertex_id) {
    GpuGeometry g=geometries[geometry_id];
    VertexWords data=VertexWords(uint64_t(g.addresses.x)|(uint64_t(g.addresses.y)<<32));
    uint k=vertex_id*g.counts.w;
    PusuSurface s;
    s.position=vec3(data.words[k],data.words[k+1],data.words[k+2]);
    s.uv=vec2(data.words[k+3],data.words[k+4]);s.light_uv=vec2(data.words[k+5],data.words[k+6]);
    s.normal=vec3(data.words[k+7],data.words[k+8],data.words[k+9]);
    s.color=vec4(data.words[k+10],data.words[k+11],data.words[k+12],data.words[k+13]);
    s.triangle_id=0xffffffffu;s.barycentrics=vec2(0);return s;
}
PusuSurface pusu_world_surface(uint instance_id,PusuSurface s) {
    GpuInstance i=instances[instance_id];s.position=(i.model*vec4(s.position,1)).xyz;
    s.normal=mat3(i.normal0.xyz,i.normal1.xyz,i.normal2.xyz)*s.normal;return s;
}
PusuSurface pusu_triangle_surface(uint triangle_id,vec2 barycentrics) {
    GpuTriangle t=triangles[triangle_id];
    PusuSurface a=pusu_vertex(t.source.y,t.vertices.x),b=pusu_vertex(t.source.y,t.vertices.y),c=pusu_vertex(t.source.y,t.vertices.z);
    vec3 w=vec3(1-barycentrics.x-barycentrics.y,barycentrics);PusuSurface s;
    s.position=a.position*w.x+b.position*w.y+c.position*w.z;s.normal=a.normal*w.x+b.normal*w.y+c.normal*w.z;
    s.uv=a.uv*w.x+b.uv*w.y+c.uv*w.z;s.light_uv=a.light_uv*w.x+b.light_uv*w.y+c.light_uv*w.z;
    s.color=a.color*w.x+b.color*w.y+c.color*w.z;s.triangle_id=triangle_id;s.barycentrics=barycentrics;
    return pusu_world_surface(t.vertices.w,s);
}
vec3 pusu_coordinates(uint pass_id,PusuSurface s,vec3 view_origin,vec3 view_direction) {
    GpuPass p=passes[pass_id];vec3 t=vec3(s.uv,0);
    if(p.binding.w==1u)t=vec3(s.light_uv,0);
    else if(p.binding.w>=2u) {
        mat3 basis=(p.modes.z&1u)!=0u?mat3(1):mat3(frame.view);
        vec3 n=normalize(basis*normalize(s.normal));
        vec3 incident=s.triangle_id==0xffffffffu?normalize(basis*(s.position-view_origin)):normalize(basis*view_direction);
        vec3 r=reflect(incident,n);
        if(p.binding.w==2u){float m=2*sqrt(r.x*r.x+r.y*r.y+(r.z+1)*(r.z+1));t=vec3(r.xy/max(m,0.00001)+0.5,0);}else t=r;
    }
    return (p.tc_matrix*vec4(t,1)).xyz;
}
vec4 pusu_sample_coordinates(uint pass_id,vec3 coordinates,float lod) {
    GpuPass p=passes[pass_id];
    if(p.binding.z!=0u) {
        return lod<0?texture(samplerCube(cubes[nonuniformEXT(p.binding.x)],samplers[nonuniformEXT(p.binding.y)]),coordinates):textureLod(samplerCube(cubes[nonuniformEXT(p.binding.x)],samplers[nonuniformEXT(p.binding.y)]),coordinates,lod);
    }
    vec2 uv=(p.modes.z&2u)!=0u?clamp(coordinates.xy,0,1):coordinates.xy;
    // Fonts retain authored geometry/UVs but may not filter adjacent atlas cells.
    if((p.modes.z&64u)!=0u)uv=clamp(uv,p.parameters.xy,p.parameters.zw);
    return lod<0?texture(sampler2D(images[nonuniformEXT(p.binding.x)],samplers[nonuniformEXT(p.binding.y)]),uv):textureLod(sampler2D(images[nonuniformEXT(p.binding.x)],samplers[nonuniformEXT(p.binding.y)]),uv,lod);
}
vec4 pusu_sample_texture(uint instance_id,uint pass_id,PusuSurface s,vec3 view_origin,vec3 view_direction,float lod) {
    vec3 coordinates;
    if(s.triangle_id!=0xffffffffu) {
        GpuTriangle t=triangles[s.triangle_id];vec3 w=vec3(1-s.barycentrics.x-s.barycentrics.y,s.barycentrics);
        PusuSurface a=pusu_world_surface(instance_id,pusu_vertex(t.source.y,t.vertices.x));
        PusuSurface b=pusu_world_surface(instance_id,pusu_vertex(t.source.y,t.vertices.y));
        PusuSurface c=pusu_world_surface(instance_id,pusu_vertex(t.source.y,t.vertices.z));
        a.triangle_id=b.triangle_id=c.triangle_id=s.triangle_id;
        vec3 ca=pusu_coordinates(pass_id,a,view_origin,view_direction),cb=pusu_coordinates(pass_id,b,view_origin,view_direction),cc=pusu_coordinates(pass_id,c,view_origin,view_direction);
        if(!pusu_finite(ca)||!pusu_finite(cb)||!pusu_finite(cc))return vec4(uintBitsToFloat(0x7fc00000u));
        coordinates=ca*w.x+cb*w.y+cc*w.z;
        if(!pusu_finite(coordinates)||(passes[pass_id].binding.z!=0u&&!(dot(coordinates,coordinates)>0)))return vec4(uintBitsToFloat(0x7fc00000u));
        if(lod<=-2) {
            GpuPass p=passes[pass_id];float footprint=-2-lod;
            vec3 e1=b.position-a.position,e2=c.position-a.position,n=cross(e1,e2);
            float d11=dot(e1,e1),d12=dot(e1,e2),d22=dot(e2,e2),det=d11*d22-d12*d12;
            if(!(det>0)||isnan(det)||isinf(det))return vec4(uintBitsToFloat(0x7fc00000u));
            footprint/=max(abs(dot(normalize(n),normalize(view_direction))),0.0001);
            vec3 dual1=(e1*d22-e2*d12)/det,dual2=(e2*d11-e1*d12)/det;
            vec3 delta1=cb-ca,delta2=cc-ca;
            float density;
            if(p.binding.z!=0u) {
                if(!(dot(ca,ca)>0&&dot(cb,cb)>0&&dot(cc,cc)>0))return vec4(uintBitsToFloat(0x7fc00000u));
                delta1=normalize(cb)-normalize(ca);delta2=normalize(cc)-normalize(ca);
                vec3 gx=dual1*delta1.x+dual2*delta2.x,gy=dual1*delta1.y+dual2*delta2.y,gz=dual1*delta1.z+dual2*delta2.z;
                float side=float(textureSize(samplerCube(cubes[nonuniformEXT(p.binding.x)],samplers[nonuniformEXT(p.binding.y)]),0).x);
                density=max(max(length(gx),length(gy)),length(gz))*side;
            }else {
                vec2 size=vec2(textureSize(sampler2D(images[nonuniformEXT(p.binding.x)],samplers[nonuniformEXT(p.binding.y)]),0));
                vec3 gx=dual1*delta1.x+dual2*delta2.x,gy=dual1*delta1.y+dual2*delta2.y;
                density=max(length(gx)*size.x,length(gy)*size.y);
            }
            float texels=footprint*density;
            if(isnan(texels)||isinf(texels))return vec4(uintBitsToFloat(0x7fc00000u));
            lod=max(0,log2(max(texels,1)));
        }
    }else coordinates=pusu_coordinates(pass_id,s,view_origin,view_direction);
    return pusu_sample_coordinates(pass_id,coordinates,lod);
}
vec4 pusu_modulate_pass(uint instance_id,uint pass_id,PusuSurface s,vec4 raw) {
    GpuPass p=passes[pass_id];vec4 entity=instances[instance_id].entity_color;
    vec3 rgb=p.constant_color.rgb;if(p.modes.x==1u)rgb=s.color.rgb;else if(p.modes.x==3u)rgb*=entity.rgb;
    float alpha=p.constant_color.a;if(p.modes.x==1u&&alpha==1)alpha=s.color.a;
    raw*=vec4(rgb,alpha*entity.a);if((p.modes.z&1u)!=0u&&p.modes.x!=3u)raw.rgb*=entity.rgb;return raw;
}
bool pusu_pass_accepts_alpha(uint pass_id,float alpha) {
    uint test=passes[pass_id].modes.y;
    return !(test==1u&&!(alpha>0)||test==2u&&!(alpha<128.0/255.0)||test==3u&&!(alpha>=128.0/255.0)||test==4u&&alpha!=0);
}
vec4 pusu_fog_pass(uint pass_id,vec4 color,float depth) {
    if((passes[pass_id].modes.z&4u)==0u)return color;
    float f=1;int mode=int(frame.fog_parameters.w);
    if(mode==2048)f=exp(-frame.fog_parameters.z*depth);
    else if(mode==2049){float d=frame.fog_parameters.z*depth;f=exp(-d*d);}
    else f=(frame.fog_parameters.y-depth)/max(frame.fog_parameters.y-frame.fog_parameters.x,0.00001);
    color.rgb=mix(frame.fog_color.rgb,color.rgb,clamp(f,0,1));return color;
}
vec3 pusu_decode(vec3 c) { return mix(c/12.92,pow((max(c,vec3(0))+0.055)/1.055,vec3(2.4)),greaterThan(c,vec3(0.04045))); }
vec3 pusu_encode(vec3 c) { return mix(c*12.92,1.055*pow(max(c,vec3(0)),vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308))); }
vec4 pusu_sample_pass(uint instance_id,uint pass_id,PusuSurface s,vec3 origin,vec3 direction,float lod) {
    vec4 c=pusu_modulate_pass(instance_id,pass_id,s,pusu_sample_texture(instance_id,pass_id,s,origin,direction,lod));
    if((passes[pass_id].modes.z&4u)==0u)return c;
    vec3 unit_direction=normalize(direction);float depth=abs(dot(s.position-origin,unit_direction));
    if(s.triangle_id!=0xffffffffu) {
        GpuTriangle t=triangles[s.triangle_id];vec3 w=vec3(1-s.barycentrics.x-s.barycentrics.y,s.barycentrics);
        vec3 a=pusu_world_surface(instance_id,pusu_vertex(t.source.y,t.vertices.x)).position;
        vec3 b=pusu_world_surface(instance_id,pusu_vertex(t.source.y,t.vertices.y)).position;
        vec3 d=pusu_world_surface(instance_id,pusu_vertex(t.source.y,t.vertices.z)).position;
        depth=dot(vec3(abs(dot(a-origin,unit_direction)),abs(dot(b-origin,unit_direction)),abs(dot(d-origin,unit_direction))),w);
    }
    return pusu_fog_pass(pass_id,c,depth);
}
vec4 pusu_blend_factor(uint f,vec4 s,vec4 d) {
    if(f==0u)return vec4(0);if(f==1u)return vec4(1);if(f==2u)return s;if(f==3u)return 1-s;
    if(f==4u)return d;if(f==5u)return 1-d;if(f==6u)return vec4(s.a);return vec4(1-s.a);
}
vec4 pusu_blend_pass(uint pass_id,vec4 source,vec4 destination) {
    GpuPass p=passes[pass_id];if((p.modes.z&32u)==0u)return source;
    return source*pusu_blend_factor(p.blend.x,source,destination)+destination*pusu_blend_factor(p.blend.y,source,destination);
}
vec4 pusu_evaluate_hit(uint instance_id,PusuSurface s,vec3 origin,vec3 direction,float lod) {
    GpuInstance i=instances[instance_id];vec4 result=vec4(0);bool depth_written=false;
    for(uint k=0;k<i.ranges.y;++k) {
        uint id=i.ranges.x+k;GpuPass p=passes[id];
        if((p.blend.z&5u)==5u&&!depth_written)continue;
        vec4 c=pusu_sample_pass(instance_id,id,s,origin,direction,lod);
        if(any(isnan(c))||any(isinf(c)))return c;
        if(!pusu_pass_accepts_alpha(id,c.a))continue;
        if(frame.flags.x!=0u)c.rgb=pusu_decode(c.rgb);
        result=pusu_blend_pass(id,c,result);
        if((p.blend.z&3u)==3u)depth_written=true;
    }
    return result;
}
#endif
