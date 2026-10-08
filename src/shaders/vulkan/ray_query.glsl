#ifndef PUSU_RAY_QUERY_GLSL
#define PUSU_RAY_QUERY_GLSL
#extension GL_EXT_ray_query : require
layout(set=0,binding=10) uniform accelerationStructureEXT pusu_tlas;
struct GpuRayInstance { uvec4 mapping; };
layout(set=0,binding=11,std430) readonly buffer RayInstances { GpuRayInstance pusu_ray_instances[]; };
layout(set=0,binding=12,std430) readonly buffer PrimitiveMap { uint pusu_primitive_map[]; };
// Per fragment only: material.frag writes this before any discard/divergence.
float pusu_ray_receiver_footprint=0.0;

bool pusu_ray_finite(float value) { return !isnan(value)&&!isinf(value); }
vec3 pusu_ray_unit(vec3 value) {
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    return pusu_finite(value)&&scale>0?normalize(value/scale):vec3(0);
}
bool pusu_ray_surface_valid(PusuSurface surface) {
    return pusu_finite(surface.position)&&pusu_finite(surface.normal)&&
           !any(isnan(surface.uv))&&!any(isinf(surface.uv))&&
           !any(isnan(surface.light_uv))&&!any(isinf(surface.light_uv))&&
           !any(isnan(surface.color))&&!any(isinf(surface.color));
}
float pusu_ray_epsilon(vec3 position) {
    // Four position ULPs, not a scene-wide fraction: detailed nearby geometry
    // remains reachable even when the admitted world/ray extent is large.
    return max(frame.quality.w,max(max(abs(position.x),abs(position.y)),abs(position.z))*4.76837158203125e-7);
}
vec3 pusu_ray_origin(vec3 position,vec3 normal,vec3 direction,float epsilon) {
    return position+normal*(dot(normal,direction)>=0?epsilon:-epsilon);
}
bool pusu_ray_triangle(in rayQueryEXT query,bool committed,out uint instance_id,out uint triangle_id) {
    uint index=committed?rayQueryGetIntersectionInstanceCustomIndexEXT(query,true):rayQueryGetIntersectionInstanceCustomIndexEXT(query,false);
    uint primitive=committed?rayQueryGetIntersectionPrimitiveIndexEXT(query,true):rayQueryGetIntersectionPrimitiveIndexEXT(query,false);
    if(index>=uint(pusu_ray_instances.length()))return false;
    uvec4 record=pusu_ray_instances[index].mapping;
    if(primitive>=record.z||record.y>=uint(pusu_primitive_map.length())||
       primitive>=uint(pusu_primitive_map.length())-record.y)return false;
    instance_id=record.x;triangle_id=pusu_primitive_map[record.y+primitive];
    return instance_id<uint(instances.length())&&triangle_id<uint(triangles.length())&&triangles[triangle_id].vertices.w==instance_id;
}
bool pusu_ray_candidate(in rayQueryEXT query,vec3 origin,vec3 direction,float footprint) {
    if(rayQueryGetIntersectionTypeEXT(query,false)!=gl_RayQueryCandidateIntersectionTriangleEXT)return false;
    uint instance_id,triangle_id;
    if(!pusu_ray_triangle(query,false,instance_id,triangle_id))return false;
    uint coverage_id=instances[instance_id].metadata.y;
    if(coverage_id>=uint(coverages.length()))return false;
    uvec4 coverage=coverages[coverage_id].passes;
    if(coverage.w==0u||coverage.x>uint(coverage_passes.length())||coverage.y>uint(coverage_passes.length())-coverage.x)return false;
    bool front=rayQueryGetIntersectionFrontFaceEXT(query,false);
    if(coverage.z==1u&&front||coverage.z==2u&&!front)return false;
    PusuSurface surface=pusu_triangle_surface(triangle_id,rayQueryGetIntersectionBarycentricsEXT(query,false));
    if(!pusu_finite(surface.position))return false;
    bool depth_written=false;
    // Every surviving unblended LEQUAL writer establishes depth. Later EQUAL
    // or discarded masks cannot create coverage through a hole in that depth.
    for(uint ordinal=0u;ordinal<coverage.y;++ordinal) {
        uint pass_id=coverage_passes[coverage.x+ordinal];
        if(pass_id>=uint(passes.length()))return false;
        GpuPass pass=passes[pass_id];
        if((pass.modes.z&32u)!=0u||(pass.blend.z&3u)!=3u)continue;
        if((pass.blend.z&4u)!=0u&&!depth_written)continue;
        // No alpha test means an actual depth-writing raster pass covers even
        // with zero alpha. Unused raw UV/normal NaNs do not erase opaque depth.
        if(pass.modes.y!=0u) {
            if(pass.binding.w>=2u&&(!pusu_finite(surface.normal)||!(dot(surface.normal,surface.normal)>0)))continue;
            vec4 color=pusu_sample_pass(instance_id,pass_id,surface,origin,direction,-2.0-max(footprint,0.0));
            if(!pusu_ray_finite(color.a)||!pusu_pass_accepts_alpha(pass_id,color.a))continue;
        }
        if((pass.blend.z&4u)==0u)depth_written=true;
    }
    return depth_written;
}
float pusu_ray_visibility(vec3 position,vec3 unit_normal,vec3 direction,float extent) {
    if(frame.flags.y==0u)return 1.0;
    vec3 normal=pusu_ray_unit(unit_normal),ray=pusu_ray_unit(direction);
    if(!pusu_finite(position)||dot(normal,normal)==0||dot(ray,ray)==0||
       !pusu_ray_finite(extent)||extent<=0||!pusu_ray_finite(pusu_ray_receiver_footprint))return 1.0;
    float epsilon=pusu_ray_epsilon(position);
    if(extent<=epsilon)return 1.0;
    vec3 origin=pusu_ray_origin(position,normal,ray,epsilon);
    if(!pusu_finite(origin))return 1.0;
    rayQueryEXT query;
    rayQueryInitializeEXT(query,pusu_tlas,gl_RayFlagsTerminateOnFirstHitEXT|gl_RayFlagsNoOpaqueEXT,
                          0xff,origin,epsilon*0.25,ray,extent);
    while(rayQueryProceedEXT(query))if(pusu_ray_candidate(query,origin,ray,pusu_ray_receiver_footprint))rayQueryConfirmIntersectionEXT(query);
    return rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT?1.0:0.0;
}
vec3 pusu_ray_reflection(uint instance_id,uint pass_id,PusuSurface surface,vec3 incident,vec3 probe_rgb,float lod) {
    if(frame.flags.z==0u||(passes[pass_id].modes.z&8u)==0u)return probe_rgb;
    vec3 normal=pusu_ray_unit(surface.normal),incoming=pusu_ray_unit(incident);
    vec3 direction=reflect(incoming,normal);
    // Derivatives precede every per-pixel exit and divergent traversal.
    float spread=max(length(dFdx(direction)),length(dFdy(direction)));
    float initial=max(length(dFdx(surface.position)),length(dFdy(surface.position)));
    if(!pusu_ray_surface_valid(surface)||dot(normal,normal)==0||dot(incoming,incoming)==0||
       !pusu_ray_finite(spread)||!pusu_ray_finite(initial))return probe_rgb;
    // Transparent receivers need not belong to the admitted-AS bounds. The
    // camera-to-AS diagonal plus camera-to-receiver distance covers them too.
    vec3 camera_delta=surface.position-frame.camera_time.xyz;
    float camera_scale=max(max(abs(camera_delta.x),abs(camera_delta.y)),abs(camera_delta.z));
    float camera_distance=camera_scale>0?length(camera_delta/camera_scale)*camera_scale:0.0;
    float epsilon=pusu_ray_epsilon(surface.position),extent=frame.light_direction.w+camera_distance;
    if(!pusu_ray_finite(extent)||extent<=epsilon)return probe_rgb;
    vec3 origin=pusu_ray_origin(surface.position,normal,direction,epsilon);
    if(!pusu_finite(origin))return probe_rgb;
    rayQueryEXT query;
    rayQueryInitializeEXT(query,pusu_tlas,gl_RayFlagsNoOpaqueEXT,0xff,origin,epsilon*0.25,direction,extent);
    while(rayQueryProceedEXT(query)) {
        float candidate_distance=rayQueryGetIntersectionTEXT(query,false);
        float candidate_footprint=initial+spread*candidate_distance;
        if(pusu_ray_finite(candidate_footprint)&&pusu_ray_candidate(query,origin,direction,candidate_footprint))rayQueryConfirmIntersectionEXT(query);
    }
    if(rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT)return probe_rgb;
    uint hit_instance,hit_triangle;
    if(!pusu_ray_triangle(query,true,hit_instance,hit_triangle))return probe_rgb;
    PusuSurface hit=pusu_triangle_surface(hit_triangle,rayQueryGetIntersectionBarycentricsEXT(query,true));
    if(!pusu_ray_surface_valid(hit))return probe_rgb;
    float distance=rayQueryGetIntersectionTEXT(query,true);
    float footprint=max(initial+spread*distance,epsilon);
    if(!pusu_ray_finite(footprint))return probe_rgb;
    // <=-2 is the shared evaluator's physical world-footprint convention;
    // -1 remains implicit raster sampling, nonnegative values explicit mip LOD.
    vec4 color=pusu_evaluate_hit(hit_instance,hit,origin,direction,-2.0-footprint);
    if(!pusu_finite(color.rgb))return probe_rgb;
    // Hit algebra accumulates linear working color in HDR mode, whereas this
    // function replaces an encoded raw sample BEFORE the receiver's algebra.
    if(frame.flags.x!=0u)color.rgb=mix(12.92*color.rgb,1.055*pow(max(color.rgb,vec3(0)),vec3(1.0/2.4))-0.055,
                                    greaterThan(color.rgb,vec3(0.0031308)));
    return color.rgb;
}
vec3 pusu_enhanced_directional(uint instance_id,PusuSurface surface,vec3 legacy_albedo_proxy) {
    if(frame.flags.w==0u||instances[instance_id].metadata.y>=uint(coverages.length()))return vec3(0);
    uint coverage_id=instances[instance_id].metadata.y;
    if(coverages[coverage_id].passes.w==0u||!pusu_ray_surface_valid(surface)||!pusu_finite(legacy_albedo_proxy))return vec3(0);
    vec3 normal=pusu_ray_unit(surface.normal),light=frame.light_direction.xyz;
    if(dot(normal,normal)==0)return vec3(0);
    float cosine=max(dot(normal,light),0.0);
    if(cosine<=0||frame.light_color.w<=0||
       !pusu_finite(pusu_ray_origin(surface.position,normal,light,pusu_ray_epsilon(surface.position)))||
       (frame.flags.y!=0u&&!pusu_ray_finite(pusu_ray_receiver_footprint)))return vec3(0);
    float visibility=pusu_ray_visibility(surface.position,normal,light,frame.light_direction.w);
    vec3 radiance=pusu_decode(max(legacy_albedo_proxy,vec3(0)))*frame.light_color.rgb*frame.light_color.w*cosine*visibility;
    // The material caller adds this exactly once on its eligible base pass;
    // nothing here multiplies/subtracts the original bake, grid or shadow stages.
    return radiance;
}
#endif
