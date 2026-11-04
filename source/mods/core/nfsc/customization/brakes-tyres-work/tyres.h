/* Private EC WheelTextures.h/Wheels.h port. Requires brakes.h helpers.
 * Four separate eModels retain native steering/suspension transforms. State is
 * keyed by CarRenderInfo; native destructor releases models before reuse.
 * No struct extension or simulation/physics dimensions are written.
 * EC tyre bounds update only CarRenderInfo WheelData render dimensions. */
#define CUST_TIRE_STATE_COUNT 32u
#define CUST_TIRE_FACTORY_ROWS 96u
#define CUST_TIRE_BYTES (240u + 4u*CUST_TIRE_FACTORY_ROWS*12u) /* original models/tables/bbox + per-wheel merged tables */
typedef struct {
    uint32_t render, ride, storage, initialized;
} CustTireState;
static CustTireState g_cust_tires[CUST_TIRE_STATE_COUNT];
static uint32_t g_cust_tire_rendering;
static uint32_t g_cust_tire_trace_render, g_cust_tire_trace_texture, g_cust_tire_trace_mask, g_cust_tire_trace_count;
static float cust_tire_float(uint32_t address) {
    uint32_t bits = get_u32(address); float value; memcpy(&value, &bits, sizeof(value)); return value;
}
static void cust_tire_write_float(uint32_t address, float value) {
    uint32_t bits; memcpy(&bits, &value, sizeof(bits)); g_api->guest_write_u32(g_api, address, bits);
}
static CustTireState *cust_tire_state(uint32_t render) {
    for (uint32_t i = 0; i < CUST_TIRE_STATE_COUNT; ++i)
        if (g_cust_tires[i].render == render) return &g_cust_tires[i];
    return NULL;
}
static uint32_t cust_tire_texture(uint32_t hash) {
    const uint32_t args[3] = {hash, 0u, 0u};
    return hash ? cust_call(0x0055cfd0u, 0u, args, 3u) : 0u;
}
static void cust_tire_entry(uint32_t table, uint32_t index, uint32_t old_name, uint32_t new_name) {
    const uint32_t texture = cust_tire_texture(new_name);
    g_api->guest_write_u32(g_api, table + index * 12u, texture ? old_name : 0u);
    g_api->guest_write_u32(g_api, table + index * 12u + 4u, texture ? new_name : 0u);
    g_api->guest_write_u32(g_api, table + index * 12u + 8u, texture ? texture : 0xffffffffu);
}
static void cust_tire_table(uint32_t table, uint32_t texture) {
    cust_tire_entry(table, 0u, 0xc2999ec7u, texture); /* WHEEL_TIRE */
    cust_tire_entry(table, 1u, 0xcf7c7914u, cust_hash_seed("_N", texture));
    cust_tire_entry(table, 2u, 0x54922dd9u, cust_hash("TIRE_BRAND_00"));
    cust_tire_entry(table, 3u, 0xe4cea024u, 0x66994f4au);
    cust_tire_entry(table, 4u, 0xe4cea025u, 0x66994f4au);
}
static void cust_tire_drop(CustTireState *s) {
    if (!s || !s->storage) return;
    for (uint32_t n = 0; n < 4u; ++n) {
        if (s->initialized & (1u << n)) cust_call(0x0055bf70u, s->storage + n * 24u, NULL, 0u);
    }
    s->render = s->ride = s->initialized = 0u;
}
static void cust_tire_init_model(CustTireState *s, uint32_t n, uint32_t radius, uint32_t model) {
    if (radius < 17u || radius > 21u) return;
    char suffix[32];
    /* Native wheel numbering FL0 FR1 RR2 RL3. */
    snprintf(suffix, sizeof(suffix), "_%s_%u_A", n == 0u || n == 3u ? "LEFT" : "RIGHT", radius);
    const uint32_t object = s->storage + n * 24u;
    const uint32_t init[1] = {cust_hash_seed(suffix, model)};
    cust_call(0x00412230u, object, NULL, 0u);
    s->initialized |= 1u << n;
    cust_call(0x0055e7e0u, object, init, 1u);
    const uint32_t table[3] = {s->storage + 96u + (n > 1u ? 60u : 0u), 5u, 0u};
    cust_call(0x005588d0u, object, table, 3u);
}
static void cust_tire_init(uint32_t render, uint32_t ride) {
    CustTireState *s = cust_tire_state(render);
    if (s) cust_tire_drop(s);
    if (!cust_bt_allowed(ride)) return;
    const uint32_t front = cust_bt_part(ride, 0x54u), rear = cust_bt_part(ride, 0x55u);
    const int front_missing_tire = !(front && cust_attr(front, 0x001d38b3u, 1u));
    const int rear_missing_tire = !(rear && cust_attr(rear, 0x001d38b3u, 1u));
    uint32_t tire = cust_bt_part(ride, 0x67u); /* EC Slot_Tires == CUSTOM_HUD */
    if (!tire && (front_missing_tire || rear_missing_tire)) tire = cust_next(0x67u, 0xffffffffu, 0u);
    if (!tire) return; /* Original integrated-tyre wheels stay original. */
    const uint32_t model = cust_attr(tire, 0x0348b290u, 0u);
    const uint32_t texture = cust_attr(tire, 0x10c98090u, 0u);
    if (!model || !texture || !cust_tire_texture(texture)) return;
    if (!s) for (uint32_t i = 0; i < CUST_TIRE_STATE_COUNT; ++i)
        if (!g_cust_tires[i].render) { s = &g_cust_tires[i]; break; }
    if (!s) { log_line("customization tyres: render state full, original wheel retained"); return; }
    if (!s->storage && g_api->guest_alloc(g_api, CUST_TIRE_BYTES, &s->storage) != POP_OK) return;
    s->render = render; s->ride = ride; s->initialized = 0u;
    cust_tire_table(s->storage + 96u, texture);
    cust_tire_table(s->storage + 156u, texture);
    uint32_t radius = 0u;
    if (front && ((!cust_attr(front, 0x03b83203u, 0u) && !cust_attr(front, 0x2850a03bu, 0u)) ||
                  !cust_attr(front, 0x001d38b3u, 1u))) {
        radius = cust_attr(front, 0xeb0101e2u, 0u);
        cust_tire_init_model(s, 0u, radius, model);
        cust_tire_init_model(s, 1u, radius, model);
    }
    if (rear && ((!cust_attr(rear, 0x03b83203u, 0u) && !cust_attr(rear, 0x2850a03bu, 0u)) ||
                 cust_attr(rear, 0x2c420d64u, 0u) || !cust_attr(rear, 0x001d38b3u, 1u)))
        radius = cust_attr(rear, 0xeb0101e2u, radius);
    if (radius) {
        cust_tire_init_model(s, 2u, radius, model);
        cust_tire_init_model(s, 3u, radius, model);
    }
    if (g_cust_tire_trace_count < 16u &&
        (g_cust_tire_trace_render != render || g_cust_tire_trace_texture != texture ||
         g_cust_tire_trace_mask != s->initialized)) {
        g_cust_tire_trace_render = render;
        g_cust_tire_trace_texture = texture;
        g_cust_tire_trace_mask = s->initialized;
        ++g_cust_tire_trace_count;
        char line[280];
        snprintf(line, sizeof line, "customization tyres: models render=%08x ride=%08x part=%u texture=%08x storage=%08x initialized=%x solids=%08x,%08x,%08x,%08x",
                 render, ride, cust_extension_index(tire), texture, s->storage, s->initialized,
                 get_u32(s->storage + 12u), get_u32(s->storage + 36u),
                 get_u32(s->storage + 60u), get_u32(s->storage + 84u));
        log_line(line);
    }
}
/* EC CarWheelTextures::AdjustWheelData: front-left and rear-left bounds.
 * Offsets confirmed in native constructor007e58d3/df/eb/f6. */
static void cust_tire_adjust_dimensions(uint32_t render) {
    CustTireState *s = cust_tire_state(render);
    if (!s) return;
    for (uint32_t side = 0; side < 2u; ++side) {
        const uint32_t n = side ? 3u : 0u, model = s->storage + n * 24u;
        if (!(s->initialized & (1u << n)) || !get_u32(model + 12u)) continue;
        const uint32_t bbox = s->storage + 216u, args[2] = {bbox, bbox + 12u};
        cust_call(0x005589c0u, model, args, 2u);
        const float width = fabsf(cust_tire_float(bbox + 4u) - cust_tire_float(bbox + 16u));
        const float radius = fabsf(cust_tire_float(bbox) - cust_tire_float(bbox + 12u)) * 0.5f;
        if (!(width > 0.0f) || !(radius > 0.0f) || !isfinite(width) || !isfinite(radius)) continue;
        cust_tire_write_float(render + 0x470u + side * 4u, width);
        cust_tire_write_float(render + 0x478u + side * 4u, radius);
    }
}
static void cust_tire_ctor(const PopModApi *api, pop_cpu_v1 *cpu,
                           PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t render = cpu->ecx, ride = get_u32(cpu->esp + 4u);
    /* EC constructs tyre models before native constructor. Its UpdateCarParts
     * call007e5a60 applies tyre bounds before later native radius scales. */
    cust_tire_init(render, ride);
    api->call_next(api, inv, cpu);
    cust_paint_init(render,ride);
}
static void cust_tire_dtor(const PopModApi *api, pop_cpu_v1 *cpu,
                           PopHookInvocation *inv, void *user) {
    (void)user;
    cust_paint_drop(cpu->ecx);
    cust_tire_drop(cust_tire_state(cpu->ecx));
    api->call_next(api, inv, cpu);
}
/* The normal front end can rebuild parts without reallocating CarRenderInfo. */
static void cust_tire_update(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t render = cpu->ecx;
    cust_paint_restore_ri(cust_paint_state(render));
    api->call_next(api, inv, cpu);
    cust_paint_init(render,get_u32(render+0x3f0u));
    if (cust_pilot_enabled() && cust_catalog_ready()) {
        cust_tire_init(render, get_u32(render + 0x3f0u));
        cust_tire_adjust_dimensions(render);
    }
}
/* Factory geometry retains its native rim and tyre mesh. Replace only the
 * two proven native tyre channels; preserve all original model replacements.
 * No extra tyre model or dimension/physics change is made on this path. */
static int cust_tire_factory_draw(CustTireState *s,uint32_t wheel,uint32_t model,
 const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv) {
 if(!s || !model || wheel>=4u || !cust_bt_allowed(s->ride))return 0;
 uint32_t front=cust_bt_part(s->ride,0x54u),rear=cust_bt_part(s->ride,0x55u);
 uint32_t part=wheel<2u?front:(rear&&!cust_attr(rear,0x2c420d64u,0u)?rear:front);
 if(!part || !cust_attr(part,0x03b83203u,0u) || !cust_attr(part,0x001d38b3u,1u))return 0;
 uint32_t solid=get_u32(model+12u),textures=solid?get_u32(solid+0x2cu):0u;
 uint32_t textureCount=solid?((get_u32(solid+0x18u)>>8u)&255u):0u,found=0u;
 if(!textures || textureCount>64u)return 0;
 for(uint32_t i=0;i<textureCount;++i){uint32_t key=get_u32(textures+i*8u);if(key==0xe4cea024u)found|=1u;if(key==0x52f745b1u)found|=2u;}
 if(found!=3u)return 0; /* Other factory UV/material layouts keep native look. */
 uint32_t texture=get_u32(s->storage+100u),normal=cust_hash_seed("_N",texture);
 uint32_t diffuseInfo=cust_tire_texture(texture),normalInfo=cust_tire_texture(normal);
 if(!diffuseInfo || get_u32(diffuseInfo+12u)!=texture)return 0;
 if(!normalInfo || get_u32(normalInfo+12u)!=normal)return 0;
 const uint32_t oldTable=get_u32(model+16u),oldCount=get_u32(model+20u)&0xffffu;
 if(oldCount>CUST_TIRE_FACTORY_ROWS-2u || (oldCount&&!oldTable))return 0;
 const uint32_t table=s->storage+240u+wheel*CUST_TIRE_FACTORY_ROWS*12u;
 uint32_t count=oldCount,seen=0u;
 /* No mapped pointer survives a native call. Keep all writes through the
  * scalar API so null/dirty/watch hooks and failure handling are unchanged. */
 {
 void *mapped=NULL;
 if(oldCount && oldTable>=0x10000u && api->guest_ptr(api,oldTable,oldCount*12u,&mapped)!=POP_OK)mapped=NULL;
 for(uint32_t i=0;i<oldCount;++i){
  uint32_t old,name,info;
  if(mapped){memcpy(&old,(const unsigned char*)mapped+i*12u,4u);memcpy(&name,(const unsigned char*)mapped+i*12u+4u,4u);memcpy(&info,(const unsigned char*)mapped+i*12u+8u,4u);}
  else{old=get_u32(oldTable+i*12u);name=get_u32(oldTable+i*12u+4u);info=get_u32(oldTable+i*12u+8u);}
  if(old==0xe4cea024u){name=texture;info=diffuseInfo;seen|=1u;}
  if(old==0x52f745b1u){name=normal;info=normalInfo;seen|=2u;}
  if(api->guest_write_u32(api,table+i*12u,old)!=POP_OK || api->guest_write_u32(api,table+i*12u+4u,name)!=POP_OK || api->guest_write_u32(api,table+i*12u+8u,info)!=POP_OK)return 0;
 }
 } /* mapped source is no longer in scope before any native call */
 const uint32_t keys[2]={0xe4cea024u,0x52f745b1u},names[2]={texture,normal},infos[2]={diffuseInfo,normalInfo};
 for(uint32_t i=0;i<2u;++i)if(!(seen&(1u<<i))){
  if(api->guest_write_u32(api,table+count*12u,keys[i])!=POP_OK || api->guest_write_u32(api,table+count*12u+4u,names[i])!=POP_OK || api->guest_write_u32(api,table+count*12u+8u,infos[i])!=POP_OK)return 0;++count;
 }
 const uint32_t attach[3]={table,count,0u},restore[3]={oldTable,oldCount,0u};
 cust_call(0x005588d0u,model,attach,3u);
 ++g_cust_tire_rendering;api->call_next(api,inv,cpu);--g_cust_tire_rendering;
 cust_call(0x005588d0u,model,restore,3u);return 1;
}
static void cust_tire_draw(uint32_t render, uint32_t wheel, uint32_t view, uint32_t model,
                           uint32_t transform, uint32_t light, uint32_t flags,
                           const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv) {
    CustPaintDrawRestore paint_restore;cust_paint_wheel_begin(render,wheel,model,&paint_restore);
    CustTireState *s = cust_tire_state(render);
    const uint32_t tire = s && wheel < 4u && (s->initialized & (1u << wheel)) ? s->storage + wheel * 24u : 0u;
    if (!tire || !get_u32(tire + 12u) || !model || !cust_bt_allowed(s->ride)) {
        if(!cust_tire_factory_draw(s,wheel,model,api,cpu,inv))api->call_next(api, inv, cpu);
        cust_paint_wheel_end(&paint_restore);return;
    }
    /* Native eModels can be shared. Restore the wheel's original table after the
     * single draw, so an unrelated car never inherits our tyre selection. */
    const uint32_t old_table = get_u32(model + 16u), old_count_pad = get_u32(model + 20u);
    const uint32_t table[3] = {s->storage + 96u + (wheel > 1u ? 60u : 0u), 5u, 0u};
    const uint32_t args[6] = {tire, transform, light, flags, 0u, 0u};
    ++g_cust_tire_rendering;
    cust_call(0x00729320u, view, args, 6u);
    --g_cust_tire_rendering;
    cust_call(0x005588d0u, model, table, 3u);
    api->call_next(api, inv, cpu);
    const uint32_t restore[3] = {old_table, old_count_pad & 0xffffu, 0u};
    cust_call(0x005588d0u, model, restore, 3u);
    cust_paint_wheel_end(&paint_restore);
}
static void cust_tire_wheel3(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t caller = get_u32(cpu->esp);
    uint32_t wheel = 4u;
    if (caller == 0x007dfefcu) wheel = 0u;
    else if (caller == 0x007e009du) wheel = 1u;
    else if (caller == 0x007e029au) wheel = 2u;
    const uint32_t model_ptr = get_u32(cpu->esp + 8u);
    const uint32_t model = model_ptr ? get_u32(model_ptr) & 0xfffffffcu : 0u;
    if (wheel == 4u || !model) { api->call_next(api, inv, cpu); return; }
    cust_tire_draw(cpu->ecx, wheel, get_u32(cpu->esp + 4u), model,
                   get_u32(cpu->esp + 12u), get_u32(cpu->esp + 16u), get_u32(cpu->esp + 20u), api, cpu, inv);
}
static void cust_tire_wheel4(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t caller = get_u32(cpu->esp);
    if (caller == 0x007e0d06u || caller == 0x007e0d6cu) {
        cust_bt_draw_scaled(api, cpu, inv); return;
    }
    if (g_cust_tire_rendering || caller != 0x007e0516u) {
        api->call_next(api, inv, cpu); return;
    }
    cust_tire_draw(cpu->esi, 3u, cpu->ecx, get_u32(cpu->esp + 4u) & 0xfffffffcu,
                   get_u32(cpu->esp + 8u), get_u32(cpu->esp + 12u), get_u32(cpu->esp + 16u), api, cpu, inv);
}
/* Merge into existing GetUsedCarTextureInfo wrapper after native call_next. */
static void cust_tire_used_textures(uint32_t output, uint32_t ride) {
    if (!output || !cust_bt_allowed(ride)) return;
    uint32_t tire = cust_bt_part(ride, 0x67u);
    const uint32_t front = cust_bt_part(ride, 0x54u), rear = cust_bt_part(ride, 0x55u);
    if (!tire && (!(front && cust_attr(front, 0x001d38b3u, 1u)) ||
                  !(rear && cust_attr(rear, 0x001d38b3u, 1u)))) tire = cust_next(0x67u, 0xffffffffu, 0u);
    if (!tire) return;
    const uint32_t texture = cust_attr(tire, 0x10c98090u, 0u);
    const uint32_t hashes[4] = {texture, cust_hash_seed("_N", texture), cust_hash("TIRE_BRAND_00"), 0x66994f4au};
    for (uint32_t h = 0; texture && h < 4u; ++h) {
        uint32_t count = get_u32(output + 0x340u), i = 0u;
        if (count >= 104u) return;
        while (i < count && get_u32(output + i * 4u) != hashes[h]) ++i;
        if (i < count) continue;
        g_api->guest_write_u32(g_api, output + count * 4u, hashes[h]);
        g_api->guest_write_u32(g_api, output + 0x340u, count + 1u);
    }
}
/* Add WRAP hook rows: 007e55e0 ctor,007d5280 dtor,007d9e20 update,
 * 007ae210 wheel3,00729320 wheel4. Every address is a translated function entry.
 * Tyre bounds update only render WheelData before native scale calculation.
 * Steering/rotations/lods and missing-solid fallback need native verification. */
