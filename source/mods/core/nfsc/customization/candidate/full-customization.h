#include "missing-title-font.h"
/* Customization integration. Include after the core helpers.
 * Main and isolated preview launchers explicitly set RECOMP_CUSTOMIZATION=1.
 * Addresses traced against the translated Carbon 1.4 functions:
 * 007d5b80 updates decal replacement textures; its call at 007d5c34 currently
 * asks 007cd930 for a part name. EC instead asks 007c9eb0 for TEXTURE_NAME.
 * 008426a0 is StandardSelectablePart::GetPrice (part pointer at +0x10).
 * Menu runtime verification and rear-slot mapping remain pending.
 */
static uint32_t g_cust_hooks[35];
static int g_cust_neon_render_ready,g_cust_neon_as_ready;
static int g_cust_stance_ready;
static int g_cust_plate_ready;
static int g_cust_driver_ready;
static int cust_driver_menu_supported(void);
static int cust_plate_menu_supported(void);
static int cust_pilot_enabled(void) {
    const char *pilot = getenv("RECOMP_CUSTOMIZATION");
    const char *profile = getenv("RECOMP_PROFILE_DIR");
    const char *lan = getenv("RECOMP_LAN");
    static const char preview_prefix[] = "/opfs/nfsc/profile-customlayout";
    static const char named_prefix[] = "/opfs/nfsc/profile-";
    if (!pilot || strcmp(pilot, "1") || !profile) return 0;
    /* Preserve the isolated preview's existing offline restriction. */
    if (!strncmp(profile, preview_prefix, sizeof(preview_prefix) - 1u))
        return !lan || !strcmp(lan, "0");
    if (lan && strcmp(lan, "0") && strcmp(lan, "1")) return 0;
    if (!strcmp(profile, "/opfs/nfsc/profile")) return 1;
    /* Match profileFolder's 1..32 ASCII letters/digits/_/- rule. Online
     * windows retain their own native profile folders and save lifecycle.
     * Peers must share the exact appended catalog checked below. */
    if (strncmp(profile, named_prefix, sizeof(named_prefix) - 1u)) return 0;
    const char *name = profile + sizeof(named_prefix) - 1u;
    unsigned length = 0u;
    for (; name[length]; ++length) {
        const unsigned char c = (unsigned char)name[length];
        if (length >= 32u || !((c >= 'A' && c <= 'Z') ||
                              (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return length != 0u;
}
/* Exact version-4 three-pack catalog; reject normal or stale assets at each boundary. */
static int cust_catalog_ready(void) {
    uint32_t fingerprint = 0;
    return look_db(&fingerprint) == 23115u && fingerprint == 0xe1e15255u;
}
static void cust_decal_texture(const PopModApi *api, pop_cpu_v1 *cpu,
                               PopHookInvocation *inv, void *user) {
    (void)user;
    if (cust_catalog_ready() && get_u32(cpu->esp) == 0x007d5c39u) {
        uint32_t texture = 0;
        if (api->guest_call(api, 0x007c9eb0u, cpu->ecx, NULL, 0, &texture) == POP_OK) {
            api->hook_return(api, cpu, texture, 0);
            return;
        }
    }
    api->call_next(api, inv, cpu);
}
static int cust_extension_part(uint32_t part);
static void cust_part_price(const PopModApi *api, pop_cpu_v1 *cpu,
                            PopHookInvocation *inv, void *user) {
    (void)user;
    if (!cust_catalog_ready()) { api->call_next(api, inv, cpu); return; }
    uint32_t attribute = 0;
    const uint32_t part = get_u32(cpu->ecx + 0x10u);
    /* Original rows retain native mode, ownership and price handling. */
    if (!part || !cust_extension_part(part)) { api->call_next(api, inv, cpu); return; }
    const uint32_t args[2] = {0x0380b632u, 0u}; /* PRICE, default attribute */
    if (part && api->guest_call(api, 0x007b00e0u, part, args, 2, &attribute) == POP_OK && attribute) {
        const int32_t price = (int32_t)get_u32(attribute + 4u);
        if (price >= 0) {
            api->hook_return(api, cpu, (uint32_t)price, 0);
            return;
        }
    }
    api->call_next(api, inv, cpu);
}

/* Menu state belongs to the existing front-end flow. 5 = front wheels, 8 = specialties. */
static uint32_t g_cust_section, g_cust_header, g_cust_bool_args;
static int g_cust_paint_ready;
static void cust_paint_visual_choice(const PopModApi*,pop_cpu_v1*);
/* Bounded private acceptance diagnostics; host counters never alter guest state. */
static uint32_t g_cust_accept_trace_count, g_cust_accept_trace_window;
/* Pending setup scopes: inject before native Finish creates edge fillers. */
static uint32_t g_cust_main_setup_menu,g_cust_as_setup_menu;
static int g_cust_main_setup_added,g_cust_as_setup_added;
static uint32_t g_cust_last_choice=8u;
static int g_cust_adding_category;
static uint32_t g_cust_brands[64], g_cust_brand_headers[64], g_cust_brand_count;
static uint32_t cust_hash_seed(const char *s, uint32_t h) {
    for (; *s; ++s) h = h * 33u + (uint8_t)*s;
    return h;
}
static uint32_t cust_hash(const char *s) { return cust_hash_seed(s, 0xffffffffu); }
#include "road-car-support.h"
static uint32_t cust_call(uint32_t fn, uint32_t self, const uint32_t *args, uint32_t count) {
    uint32_t result = 0;
    if (g_api->guest_call(g_api, fn, self, args, count, &result) != POP_OK) return 0;
    return result;
}
static uint32_t cust_attr(uint32_t part, uint32_t hash, uint32_t fallback) {
    const uint32_t args[2] = {hash, fallback};
    return cust_call(0x007c27c0u, part, args, 2);
}
static uint32_t cust_next(uint32_t slot, uint32_t car, uint32_t previous) {
    const uint32_t args[6] = {0u, car, slot, 0u, previous, 0xffffffffu};
    return cust_call(0x007cddf0u, 0x00b76690u, args, 6);
}
/* Original traversal completes before the private extension is appended.
 * Once a cursor enters the extension, never restart the original traversal:
 * the original function can resume another model group and repeat old rows. */
/* Append generic imported cosmetics without changing original record indexes.
 * The native group traversal may find imported records itself; a cursor already
 * in an extension must never restart native traversal and repeat old records. */
#include "body-model-eligibility.h"
static int cust_extension_slot(uint32_t slot) {
 return (slot>=0x32u && slot<=0x37u) || slot==0x1eu || slot==0x1fu || slot==0x28u || slot==0x29u || slot==0x48u || slot==0x49u || slot==0x4au || slot==0x4bu || slot==0x4cu || slot==0x22u || slot==0x2cu || slot==0x5fu || slot==0x4eu || slot==0x30u || slot==0x2fu || slot==0x54u || slot==0x55u || slot==0x64u || slot==0x65u ||
        slot==0x18u || slot==0x19u || slot==0x23u || slot==0x24u || slot==0x66u || slot==0x67u || slot==0x68u || slot==0x69u || slot==0x56u || slot==0x2du || slot==0x5eu;
}
static uint32_t cust_extension_index(uint32_t part) {
 for(uint32_t n=get_u32(0x00b76690u),k=0;n && n!=0x00b76690u && k<64u;n=get_u32(n),++k) {
  const uint32_t records=get_u32(n+0x34u),count=get_u32(n+0x38u),base=get_u32(n+0x3cu);
  if(part>=records && part<records+count*4u && (part-records)%4u==0u) {
   const uint32_t index=base+(part-records)/4u;
   return index>=20734u && index!=20738u ? index:0xffffffffu;
  }
 }
 return 0xffffffffu;
}
static int cust_extension_part(uint32_t part) { return cust_extension_index(part)!=0xffffffffu; }
static uint32_t cust_extension_next(uint32_t slot,uint32_t name,uint32_t previous,uint32_t usage,uint32_t car) {
 const uint32_t group=get_u32(0x00a73398u+slot*4u);
 uint32_t first=20734u;
 if(previous) {first=cust_extension_index(previous);if(first==0xffffffffu)return 0u;++first;}
 for(uint32_t n=get_u32(0x00b76690u),k=0;n && n!=0x00b76690u && k<64u;n=get_u32(n),++k) {
  const uint32_t records=get_u32(n+0x34u),count=get_u32(n+0x38u),base=get_u32(n+0x3cu);
  if(base+count<=first)continue;
  for(uint32_t i=first>base?first-base:0u;i<count;++i) {
   const uint32_t part=records+i*4u,index=base+i;
   if(index==20738u)continue;
   const uint32_t id=cust_attr(part,0x9239cf16u,0u);
   if(((id>>8u)&255u)!=group)continue;
   if(((slot>=0x32u && slot<=0x37u) || slot==0x1eu || slot==0x1fu || slot==0x28u || slot==0x29u || slot==0x48u || slot==0x49u || slot==0x4au || slot==0x4bu || slot==0x4cu || slot==0x22u || slot==0x2cu || slot==0x4eu || slot==0x30u || slot==0x2fu || slot==0x2du || (slot==0x55u && cust_attr(part,0x03b83203u,0u) && !cust_attr(part,0x2c420d64u,0u))) && !cust_body_model_matches(part,slot,car))continue;
   if(name && cust_call(0x007cd930u,part,NULL,0u)!=name)continue;
   if(usage!=0xffffffffu && ((id>>5u)&7u)!=usage)continue;
   return part;
  }
 }
 return 0;
}
#include "mirror-pair-proposal.h"
static int cust_vector_resources_install(void);
static unsigned g_cust_original_iterator;
#include "generic-source29.h"
static void cust_iterator(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)user;
 if(g_cust_original_iterator) {api->call_next(api,inv,cpu);return;}
 const int32_t car=(int32_t)get_u32(cpu->esp+8u);
 const uint32_t slot=get_u32(cpu->esp+12u),name=get_u32(cpu->esp+16u);
 const uint32_t prev=get_u32(cpu->esp+20u),usage=get_u32(cpu->esp+24u);
 const uint32_t effective=car==-1?cust_car_id():(uint32_t)car;
 int eligible=cust_catalog_ready() && cust_car_supported(effective) && cust_extension_slot(slot);
 if(slot==0x64u && !cust_front_decal_supported(effective))eligible=0;
 if(slot==0x65u && !cust_rear_decal_supported(effective))eligible=0;
 if(!eligible || (slot==0x5eu&&!cust_pilot_enabled())) {api->call_next(api,inv,cpu);return;}
 if(slot==0x5eu){if(get_u32(cpu->esp+4u)){api->call_next(api,inv,cpu);return;}api->hook_return(api,cpu,cust_generic_source_next(effective,name,prev,usage),24u);return;}
 const int extensions_ready=slot!=0x5fu || cust_vector_resources_install();
 if(cust_extension_part(prev)) {
  api->hook_return(api,cpu,extensions_ready?cust_extension_next(slot,name,prev,usage,effective):0u,24u);return;
 }
 /* Keep native original-part ordering, then append imports exactly once.
  * Native traversal prioritizes model groups, not physical indices. A new
  * car-specific stock part can precede an older generic imported rim. Never
  * enter the physical extension order midway through native model ordering. */
 uint32_t args[6]={get_u32(cpu->esp+4u),(uint32_t)car,slot,name,prev,usage};
 uint32_t result=0u;
 ++g_cust_original_iterator;
 for(uint32_t seen=0;seen<21579u;++seen) {
  result=cust_call(0x007cddf0u,cpu->ecx,args,6u);
  if(!result || !cust_extension_part(result))break;
  args[4]=result;
 }
 --g_cust_original_iterator;
 if(!result && extensions_ready)result=cust_extension_next(slot,name,0u,usage,effective);
 api->hook_return(api,cpu,result,24u);
}
static void cust_add_option(uint32_t menu, uint32_t header, uint32_t value) {
    const uint32_t allocation[1] = {0x98u};
    const uint32_t option = cust_call(0x006a1560u, 0, allocation, 1);
    if (!option) return;
    const uint32_t args[4] = {header, value, g_cust_bool_args, g_cust_bool_args + 4u};
    cust_call(0x005869c0u, option, args, 4);
    const uint32_t add[1] = {option};
    cust_call(0x005b2320u, menu, add, 1);
}
static void cust_main_setup(const PopModApi *api, pop_cpu_v1 *cpu,
                            PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t menu = cpu->ecx, caller = get_u32(cpu->esp);
    const int eligible=caller==0x0085a494u && cust_catalog_ready() && cust_supported();
    const uint32_t saved_menu=g_cust_main_setup_menu;
    const int saved_added=g_cust_main_setup_added;
    if(eligible) {g_cust_main_setup_menu=menu;g_cust_main_setup_added=0;g_cust_section=0;}
    api->call_next(api, inv, cpu);
    g_cust_main_setup_menu=saved_menu;g_cust_main_setup_added=saved_added;
    if(!eligible)return;
    const uint32_t choices[9]={2u,1u,0u,3u,4u,5u,6u,8u,7u};
    for(uint32_t i=0;i<(g_cust_paint_ready?9u:8u);++i)if(choices[i]==g_cust_last_choice){const uint32_t args[1]={i};cust_call(0x005c5ed0u,menu,args,1u);break;}
}
/* Observe the whole input entry before native sender/focus gates. */
static void cust_accept_input_trace(const PopModApi *api,pop_cpu_v1 *cpu,
                                    PopHookInvocation *inv,void *user) {
    (void)api;(void)inv;(void)user;
    static uint32_t count;
    const uint32_t event=get_u32(cpu->esp+4u);
    if(count>=64u || (event!=0x0c407210u && event!=0x406415e3u))return;
    const uint32_t menu=cpu->ecx,option=get_u32(menu+0x170u);
    const uint32_t object=option ? get_u32(option+0x10u):0u;
    const uint32_t sender=get_u32(cpu->esp+8u);
    char line[320];
    snprintf(line,sizeof line,"PRIVATE accept input caller=%08x event=%08x self=%08x sender=%08x option=%08x object=%08x objecthash=%08x senderhash=%08x subfocus=%u mainfocus=%u flags=%08x index=%u",
             get_u32(cpu->esp),event,menu,sender,option,object,
             object ? get_u32(object+0x10u):0u,
             sender>=0x10000u && sender<0x0ffffff0u ? get_u32(sender+0x10u):0u,
             get_u32(menu+0x249u)&255u,get_u32(menu+0x10du)&255u,
             get_u32(menu+0x120u),get_u32(menu+0x194u));
    log_line(line);++count;
}
/* Real SEH function entry 00868390: argument 1 is the selected menu value.
 * Requires explicit entry recovery; never hook its interior 008683c9 block. */
static void cust_main_choice(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (g_cust_accept_trace_count < 64u) {
        const uint32_t menu=get_u32(0x00bbaac4u);
        const uint32_t option=menu ? get_u32(menu+0x170u):0u;
        const uint32_t selected=get_u32(0x00bbb03cu);
        char line[320];
        snprintf(line,sizeof line,"PRIVATE accept entry self=%08x type=%u choice=%u section=%u menu=%08x option=%08x grey=%u selected=%08x part=%08x index=%u cart=%u",
                 cpu->ecx,get_u32(cpu->ecx+4u),get_u32(cpu->esp+4u),g_cust_section,menu,option,
                 option ? get_u32(option+0x20u)&255u:255u,selected,
                 selected ? get_u32(selected+0x10u):0u,
                 selected ? cust_extension_index(get_u32(selected+0x10u)):0xffffffffu,
                 get_u32(0x00bbaba8u+0x490u));
        log_line(line);
        ++g_cust_accept_trace_count;
        g_cust_accept_trace_window=8u;
    }
    cust_paint_visual_choice(api,cpu);
    if (!cust_catalog_ready() || !cust_supported()) { g_cust_section = 0; return; }
    if (get_u32(cpu->ecx + 4u) != 9u) return; /* only customization main screen */
    const uint32_t choice = get_u32(cpu->esp + 4u);
    if(choice==7u && !g_cust_paint_ready)return;
    g_cust_section = choice;
    g_cust_last_choice = choice;
    if(choice==7u && g_cust_paint_ready){api->guest_write_u32(api,cpu->esp+4u,3u);return;}
    if (choice == 5u || choice == 6u || choice == 8u) api->guest_write_u32(api, cpu->esp + 4u, 0u);
}
static void cust_add_part_category(uint32_t menu, uint32_t header, uint32_t slot, uint32_t filter) {
    const uint32_t args[4] = {header, slot, (uint32_t)((get_u32(menu+0x2d4u)&255u)!=0u), filter};
    const uint32_t old_pure_handler = get_u32(0x10051524u);
    const int trace_pure = get_u32(0x10000000u) == 0x00905a4du && !old_pure_handler;
    if (trace_pure) g_api->guest_write_u32(g_api, 0x10051524u, 0x004ab0e0u);
    ++g_cust_adding_category;
    cust_call(0x0085fe30u, menu, args, 4);
    --g_cust_adding_category;
    if (trace_pure) g_api->guest_write_u32(g_api, 0x10051524u, old_pure_handler);
}
/* Called by the existing005c5ef0 visibility wrapper BEFORE its original call.
 * Main83fae0 finishes at83fc91(return83fc94). AS866070 finishes via
 * 5c65c0: return5c65c8, savedESI, then outerreturn86623c atESP+8.
 * Native5b1ec0 appends subtype4 edge fillers; never append real choices after it.
 * Set added before guest calls so nested/repeated Finish cannot duplicate them. */
static void cust_menu_finish_before(pop_cpu_v1 *cpu) {
    const uint32_t menu=cpu->ecx,caller=get_u32(cpu->esp);
    if(!cust_catalog_ready() || !cust_supported())return;
    if(menu==g_cust_main_setup_menu && !g_cust_main_setup_added && caller==0x0083fc94u) {
        g_cust_main_setup_added=1;
        cust_add_option(menu,cust_hash("CUST_MAINMENU_FRONT_WHEELS"),5u);
        cust_add_option(menu,cust_hash("CUST_MAINMENU_REAR_WHEELS"),6u);
        cust_add_option(menu,cust_hash("CUST_MAINMENU_SPECIALTIES"),8u);
        if(g_cust_paint_ready)cust_add_option(menu,cust_hash("CUST_MAINMENU_PAINTS"),7u);
    } else if(menu==g_cust_body_setup_menu && !g_cust_body_setup_added && caller==0x005c65c8u && get_u32(cpu->esp+8u)==0x0086623cu && g_cust_section==0u && !(get_u32(menu+0x2d4u)&255u) && (cust_body_mirror_supported(cust_car_id())||cust_body_tail_supported(cust_car_id()))) {
        g_cust_body_setup_added=1;
        if(cust_body_mirror_supported(cust_car_id()))cust_add_part_category(menu,cust_hash("CUST_PARTS_MIRRORS"),0x22u,0u);
        if(cust_body_tail_supported(cust_car_id()))cust_add_part_category(menu,cust_hash("CUST_PARTS_BRAKELIGHTS"),0x1eu,0u);
    } else if(menu==g_cust_as_setup_menu && !g_cust_as_setup_added &&
              caller==0x005c65c8u && get_u32(cpu->esp+8u)==0x0086623cu &&
              (g_cust_neon_as_ready || g_cust_stance_ready || g_cust_plate_ready) && g_cust_section==1u &&
              (get_u32(menu+0x2d4u)&255u)) {
        g_cust_as_setup_added=1;
        if(cust_body_roof_supported(cust_car_id())&&!cust_body_has_category(menu,0x4cu))cust_add_part_category(menu,cust_hash("CUST_PARTS_ROOF"),0x4cu,0u);
        if(g_cust_neon_as_ready)cust_add_part_category(menu,cust_hash("CUST_PARTS_NEON"),0x68u,0u);
        if(g_cust_stance_ready)cust_add_part_category(menu,cust_hash("CUST_PARTS_STANCE"),0x69u,0u);
        if(g_cust_plate_ready && cust_plate_menu_supported())cust_add_part_category(menu,cust_hash("CUST_PARTS_PLATES"),0x56u,0u);
    }
}
#include "custom-wheel-size-proposal.h"
static void cust_parts_setup(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t menu = cpu->ecx;
    if(g_cust_section==0u && !(get_u32(menu+0x2d4u)&255u) && (cust_body_mirror_supported(cust_car_id())||cust_body_tail_supported(cust_car_id()))){
        const uint32_t old_menu=g_cust_body_setup_menu;const int old_added=g_cust_body_setup_added;
        g_cust_body_setup_menu=menu;g_cust_body_setup_added=0;
        api->call_next(api,inv,cpu);
        g_cust_body_setup_menu=old_menu;g_cust_body_setup_added=old_added;return;
    }
    if (cust_catalog_ready() && cust_supported() && (g_cust_neon_as_ready || g_cust_stance_ready || g_cust_plate_ready) && g_cust_section==1u && (get_u32(menu+0x2d4u)&255u)) {
        const uint32_t saved_menu=g_cust_as_setup_menu;
        const int saved_added=g_cust_as_setup_added;
        g_cust_as_setup_menu=menu;g_cust_as_setup_added=0;
        api->call_next(api,inv,cpu);
        g_cust_as_setup_menu=saved_menu;g_cust_as_setup_added=saved_added;
        return;
    }
    if (!cust_catalog_ready() || !cust_supported() || (get_u32(menu + 0x2d4u) & 0xffu) ||
        (g_cust_section != 5u && g_cust_section != 6u && g_cust_section != 8u)) {
        api->call_next(api, inv, cpu); return;
    }
    log_line("customization pilot: parts setup begin");
    const uint32_t title[2] = {get_u32(menu + 0x2c8u), cust_hash("CUST_INSTALL")};
    cust_call(0x00583a90u, 0, title, 2);
    log_line("customization pilot: title ready");
    if (g_cust_section == 8u) {
        cust_add_part_category(menu, cust_hash("CUST_PARTS_BRAKES"), 0x18u, 0u);
        cust_add_part_category(menu, cust_hash("CUST_PARTS_ROTORS"), 0x19u, 0u);
        cust_add_part_category(menu, cust_hash("CUST_PARTS_TIRES"), 0x67u, 0u);
        if(cust_driver_menu_supported())cust_add_part_category(menu,cust_hash("CUST_PARTS_DRIVERS"),0x2du,0u);
        cust_add_part_category(menu,cust_hash("CART_ITEM_NAME_VINYLS"),0x5eu,0u);
        if(g_cust_neon_render_ready)cust_add_part_category(menu,cust_hash("CUST_PARTS_NEON"),0x68u,0u);
        if (cust_front_decal_supported(cust_car_id()))
            {cust_add_part_category(menu, cust_hash("CUST_PARTS_FRONT_DECAL_WHITE"), 0x64u, 0u);
             cust_add_part_category(menu, cust_hash("CUST_PARTS_FRONT_DECAL_BLACK"), 0x64u, 0u);}
        if (cust_rear_decal_supported(cust_car_id()))
            {cust_add_part_category(menu, cust_hash("CUST_PARTS_REAR_DECAL_WHITE"), 0x65u, 0u);
             cust_add_part_category(menu, cust_hash("CUST_PARTS_REAR_DECAL_BLACK"), 0x65u, 0u);}
    } else {
        const uint32_t wheel_radius=cust_wheel_size_initial(menu,g_cust_section==6u?0x55u:0x54u);
        g_cust_brand_count = 0;
        g_cust_brands[0] = cust_hash("STOCK");
        g_cust_brand_headers[0] = 0x28c09ccbu;
        g_cust_brand_count = 1;
        uint32_t part = 0;
        for (uint32_t seen = 0; seen < 4096u; ++seen) {
            part = cust_next(g_cust_section==6u?0x55u:0x54u, 0xffffffffu, part);
            if (!part) break;
            if (cust_attr(part, 0x2850a03bu, 0u)) continue; /* autosculpt */
            const uint32_t brand = cust_attr(part, 0xebb03e66u, 0u);
            if (!brand) continue;
            uint32_t i = 0;
            while (i < g_cust_brand_count && g_cust_brands[i] != brand) ++i;
            if (i == g_cust_brand_count && i < 64u) {
                g_cust_brands[i] = brand;
                g_cust_brand_headers[i] = cust_hash_seed("_WHEEL_BRAND", brand);
                ++g_cust_brand_count;
            }
        }
        for (uint32_t i = 0; i < g_cust_brand_count; ++i)
            cust_add_part_category(menu, g_cust_brand_headers[i], g_cust_section == 6u ? 0x55u : 0x54u, wheel_radius);
    }
    log_line("customization pilot: categories ready");
    cust_call(0x005c65c0u, menu, NULL, 0);
    log_line("customization pilot: finish options returned");
    const uint32_t initial[1] = {0u};
    cust_call(0x005c6560u, menu, initial, 1);
    log_line("customization pilot: initial option returned");
    api->hook_return(api, cpu, 0u, 0u);
}
static void cust_sub_options(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)user;
    cust_wheel_size_normalize(cpu);
    const uint32_t saved = g_cust_header;
    g_cust_header = 0;
    if (cust_catalog_ready() && cust_supported() && (g_cust_section == 5u || g_cust_section == 6u || g_cust_section == 8u)) {
        const uint32_t option = get_u32(cpu->esp + 4u);
        if (option) g_cust_header = get_u32(option + 0x2cu);
    }
    log_line("customization pilot: populate sub-options begin");
    api->call_next(api, inv, cpu);
    log_line("customization pilot: populate sub-options returned");
    g_cust_header = saved;
}
static void cust_wheel_list(const PopModApi *api, pop_cpu_v1 *cpu,
                            PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t slot = get_u32(cpu->esp + 4u), head = get_u32(cpu->esp + 8u);
    uint32_t radius = get_u32(cpu->esp + 20u), brand = 0;
    if (cust_catalog_ready() && cust_supported() && (g_cust_section == 5u || g_cust_section == 6u) && (slot == 0x54u || slot == 0x55u)) {
        for (uint32_t i = 0; i < g_cust_brand_count; ++i)
            if (g_cust_brand_headers[i] == g_cust_header) brand = g_cust_brands[i];
    }
    const int generic = cust_catalog_ready() && cust_supported() && g_cust_section == 8u &&
        (slot == 0x64u || slot == 0x65u || slot == 0x18u || slot == 0x19u || slot == 0x67u || slot == 0x68u || slot==0x5eu);
    if (!brand && !generic) { api->call_next(api, inv, cpu); return; }
    log_line("customization pilot: enumerating pilot parts");
    if (radius < 17u || radius > 21u) radius = 17u;
    uint32_t part = 0, added = 0;
    for (uint32_t seen = 0; seen < 4096u; ++seen) {
        part = cust_next(slot, cust_car_id(), part);
        if (!part) break;
        if (generic) {
            if (slot==0x64u || slot==0x65u) {
                if(!cust_extension_part(part))continue;
                const int black=g_cust_header==cust_hash("CUST_PARTS_FRONT_DECAL_BLACK") || g_cust_header==cust_hash("CUST_PARTS_REAR_DECAL_BLACK");
                if(!cust_attr(part,0x03b83203u,0u) && !!cust_attr(part,0x721aff7cu,0u)!=black)continue;
            }
            if (cust_attr(part, 0x2850a03bu, 0u)) continue;
        } else if (brand == cust_hash("STOCK")) {
            if (!cust_attr(part, 0x03b83203u, 0u)) continue;
        } else if (cust_attr(part, 0xebb03e66u, 0u) != brand ||
                   cust_attr(part, 0xeb0101e2u, 0u) != radius ||
                   cust_attr(part, 0x2850a03bu, 0u)) continue;
        const uint32_t allocation[1] = {0x1cu};
        const uint32_t object = cust_call(0x006a1560u, 0, allocation, 1);
        if (!object) break;
        void *p = NULL;
        if (api->guest_ptr(api, object, 0x1cu, &p) != POP_OK || !p) break;
        memset(p, 0, 0x1c);
        api->guest_write_u32(api, object, 0x009f9cf0u);
        api->guest_write_u32(api, object + 0xcu, 0x881f8efau);
        api->guest_write_u32(api, object + 0x10u, part);
        api->guest_write_u32(api, object + 0x14u, 0x009f9cd8u);
        api->guest_write_u32(api, object + 0x18u, slot);
        const uint32_t node = object + 4u, tail = get_u32(head + 4u);
        api->guest_write_u32(api, node, head);
        api->guest_write_u32(api, node + 4u, tail);
        api->guest_write_u32(api, tail, node);
        api->guest_write_u32(api, head + 4u, node);
        ++added;
    }
    char list_msg[128];
    snprintf(list_msg, sizeof(list_msg), "customization pilot: list slot=%u header=%08x count=%u", slot, g_cust_header, added);
    log_line(list_msg);
    api->hook_return(api, cpu, 0u, 0u); /* cdecl: caller removes five arguments */
}
static void cust_category(const PopModApi *api, pop_cpu_v1 *cpu,
                          PopHookInvocation *inv, void *user) {
    (void)user;
    if (cust_catalog_ready()) {
        const uint32_t slot=get_u32(cpu->ecx+0x18u);
        const char *label=slot==0x55u?"CUST_MAINMENU_REAR_WHEELS":slot==0x64u?"CUST_PARTS_FRONT_DECAL":slot==0x65u?"CUST_PARTS_REAR_DECAL":
            slot==0x18u?"CUST_PARTS_BRAKES":slot==0x19u?"CUST_PARTS_ROTORS":slot==0x67u?"CUST_PARTS_TIRES":slot==0x68u?"CUST_PARTS_NEON":slot==0x2du?"CUST_PARTS_DRIVERS":slot==0x5eu?"CART_ITEM_NAME_VINYLS":NULL;
        if(label) {api->hook_return(api,cpu,cust_hash(label),0u);return;}
    }
    api->call_next(api, inv, cpu);
}
/* EC FeCustomizePartsMenu.h selects by unique part hash. Keep the original
 * functions' stack cleanup, and override only the three traced shop callers. */
static int cust_identity_menu(void) {
    return cust_catalog_ready() && cust_supported() &&
           (g_cust_section == 1u || g_cust_section == 5u || g_cust_section == 6u || g_cust_section == 8u);
}
static int cust_identity_slot(uint32_t slot) {
    return (g_cust_section == 1u && (slot == 0x68u || slot==0x2fu || slot==0x69u || slot==0x56u)) ||
           (g_cust_section == 5u && slot == 0x54u) ||
           (g_cust_section == 6u && slot == 0x55u) ||
           (g_cust_section == 8u && (slot==0x64u || slot==0x65u || slot==0x18u || slot==0x19u || slot==0x67u || slot==0x68u || slot==0x2du || slot==0x5eu));
}
static void cust_installed_identity(const PopModApi *api, pop_cpu_v1 *cpu,
                                     PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t record = get_u32(cpu->esp + 4u), slot = get_u32(cpu->esp + 8u);
    const int eligible = get_u32(cpu->esp) == 0x0085fa98u &&
                         cust_identity_menu() && cust_identity_slot(slot);
    api->call_next(api, inv, cpu);
    if (!eligible || !record) return;
    const uint32_t args[2] = {cust_car_id(), slot};
    const uint32_t part = cust_call(0x004ae350u, record, args, 2);
    cpu->eax = part ? cust_call(0x007cd930u, part, NULL, 0) :
                     (slot==0x5eu?cust_hash("PART_NONE"):cust_hash("CUST_PART_MISSINGSTRING"));
}
static void cust_record_identity(const PopModApi *api, pop_cpu_v1 *cpu,
                                  PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t part = cpu->ecx;
    int eligible = part && get_u32(cpu->esp) == 0x0085fc1fu && cust_identity_menu();
    if(eligible && g_cust_section==1u) {
        const uint32_t group=(cust_attr(part,0x9239cf16u,0u)>>8u)&255u;
        eligible=group==get_u32(0x00a73398u+0x68u*4u) || group==get_u32(0x00a73398u+0x2fu*4u) || group==get_u32(0x00a73398u+0x69u*4u) || group==get_u32(0x00a73398u+0x56u*4u);
    }
    api->call_next(api, inv, cpu);
    if (eligible) cpu->eax = g_cust_section==8u && ((cust_attr(part,0x9239cf16u,0u)>>8u)&255u)==98u && cust_attr(part,0x03b83203u,0u) ? cust_hash("PART_NONE") : cust_call(0x007cd930u, part, NULL, 0);
}
static void cust_selectable_identity(const PopModApi *api, pop_cpu_v1 *cpu,
                                      PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t vtable=get_u32(cpu->ecx),selectable_name=get_u32(cpu->ecx+0xcu);
    const uint32_t caller = get_u32(cpu->esp), part = get_u32(cpu->ecx + 0x10u), label_slot=get_u32(cpu->ecx+0x18u);
    const int eligible = (caller == 0x0085fa68u || caller == 0x0085fc36u) &&
                         cust_identity_menu() && cust_identity_slot(get_u32(cpu->ecx + 0x18u));
    api->call_next(api, inv, cpu);
    /* Restore exact source LANGUAGEHASH for native display-name calls, not
     * the two explicitly identity-taking source call sites. Covers list/cart. */
    if(caller!=0x0085fa68u && caller!=0x0085fc36u && label_slot==0x5eu &&
       part && vtable==0x009f9cf0u && selectable_name==0x881f8efau && cust_pilot_enabled() &&
       cust_catalog_ready() && cust_supported()) {
        const uint32_t label=cust_generic_source_label(part);
        if(label){cpu->eax=label;return;}
    }
    /* The native85FB41 caller consumes the visible AS list label. */
    if(caller==0x0085fb41u && label_slot==0x56u && part &&
       vtable==0x009f9f48u && cust_pilot_enabled() &&
       cust_catalog_ready() && cust_supported() && g_cust_section==1u &&
       cust_call(0x007cd930u,part,NULL,0u)==cust_hash("LICENSE_PLATE_STYLE01"))
        cpu->eax=cust_hash("COMMON_STOCK");
    else if (eligible && label_slot==0x5eu && part && cust_attr(part,0x03b83203u,0u))cpu->eax=cust_hash("PART_NONE");
    else if (eligible) cpu->eax = part ? ((label_slot==0x56u && cust_call(0x007cd930u,part,NULL,0u)==cust_hash("LICENSE_PLATE_STYLE01"))?cust_hash("COMMON_STOCK"):cust_call(0x007cd930u, part, NULL, 0)) : 0u;
    else if(part && cust_catalog_ready() && cust_supported() && cust_extension_part(part)) {
        const uint32_t slot=label_slot;
        if(((slot==0x18u || slot==0x19u || slot==0x23u || slot==0x24u) && !cust_attr(part,0x796c0cb0u,0u)) || (slot==0x55u && cust_attr(part,0x03b83203u,0u) && !cust_attr(part,0x2c420d64u,0u)))
            cpu->eax=cust_hash("COMMON_STOCK");
        else if(cust_call(0x007cd930u,part,NULL,0u)==cust_hash("NEON_PRESET_GREEN"))
            cpu->eax=cust_hash("CUST_NEON_GREEN_PRESET");
    }
}
static void cust_category_option_trace(const PopModApi *api, pop_cpu_v1 *cpu,
                                       PopHookInvocation *inv, void *user) {
    (void)user;
    if (g_cust_adding_category) log_line("customization pilot: native add option begin");
    api->call_next(api, inv, cpu);
    if (g_cust_adding_category) log_line("customization pilot: native add option returned");
}
static void cust_category_carbon_trace(const PopModApi *api, pop_cpu_v1 *cpu,
                                       PopHookInvocation *inv, void *user) {
    (void)user;
    if (g_cust_adding_category) log_line("customization pilot: installed carbon check begin");
    api->call_next(api, inv, cpu);
    if (g_cust_adding_category) log_line("customization pilot: installed carbon check returned");
}
/* Diagnostic _purecall handler: 004ab0e0 is an existing listed bare RET.
 * Log the original virtual-call return address, then retain the CRT's fatal path. */
static void cust_purecall_trace(const PopModApi *api, pop_cpu_v1 *cpu,
                                PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    if (!g_cust_adding_category || get_u32(cpu->esp) != 0x10025bedu) return;
    char msg[256];
    snprintf(msg, sizeof(msg), "customization pilot: purecall caller=%08x this=%08x stack=%08x %08x %08x %08x",
             get_u32(cpu->esp + 4u), cpu->ecx, get_u32(cpu->esp + 8u),
             get_u32(cpu->esp + 12u), get_u32(cpu->esp + 16u), get_u32(cpu->esp + 20u));
    log_line(msg);
}
/* Private port of Unlimiter FEPackage.h / _FNGFixes.ini for this package.
 * Clone the original UI objects; preserve their layout, children and styling. */
static uint32_t cust_ui_find(uint32_t package, const char *prefix, uint32_t number) {
    char name[80]; snprintf(name, sizeof(name), "%s%u", prefix, number);
    const uint32_t args[1] = {cust_hash(name)};
    return cust_call(0x005f3760u, package, args, 1);
}
#include "../underglow-work/parts-ui-visibility-proposal.h"
/* Find the actual FEMinList that owns an object. The native hash lookup is
 * recursive: SUB_OPTION_ and OPTION_ can be children, not package roots. */
static uint32_t cust_ui_owner_list(uint32_t list, uint32_t target,
                                   uint32_t depth, uint32_t *visited) {
    if (!list || depth > 16u || *visited >= 4096u) return 0u;
    for (uint32_t object = get_u32(list + 8u); object && *visited < 4096u;
         object = get_u32(object + 4u)) {
        ++*visited;
        if (object == target) return list;
        if (get_u32(object) == 0x009d7654u) {
            const uint32_t child_list = cust_ui_owner_list(object + 0x5cu, target,
                                                           depth + 1u, visited);
            if (child_list) return child_list;
        }
    }
    return 0u;
}
/* Package Startup resolves its authored focus GUID array into object pointers.
 * A clone must join that native array only when its template was registered. */
static int cust_ui_register_focus(uint32_t package, uint32_t source, uint32_t clone) {
    const uint32_t old_array = get_u32(package + 0x90u);
    const uint32_t count = get_u32(package + 0x94u);
    if (!old_array || !count) return 1;
    if (count >= 512u) return 0;
    void *old_memory = NULL;
    if (g_api->guest_ptr(g_api, old_array, count * 4u, &old_memory) != POP_OK || !old_memory) return 0;
    int registered = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t object = get_u32(old_array + i * 4u);
        if (object == clone) return 1;
        if (object == source) registered = 1;
    }
    if (!registered) return 1;
    const uint32_t allocate[1] = {(count + 1u) * 4u};
    const uint32_t new_array = cust_call(0x006a1560u, 0u, allocate, 1u);
    if (!new_array) return 0;
    const uint32_t free_new[1] = {new_array};
    void *new_memory = NULL;
    if (g_api->guest_ptr(g_api, new_array, (count + 1u) * 4u, &new_memory) != POP_OK || !new_memory) {
        cust_call(0x006a15a0u, 0u, free_new, 1u); return 0;
    }
    memcpy(new_memory, old_memory, count * 4u);
    if (g_api->guest_write_u32(g_api, new_array + count * 4u, clone) != POP_OK ||
        g_api->guest_write_u32(g_api, package + 0x90u, new_array) != POP_OK) {
        cust_call(0x006a15a0u, 0u, free_new, 1u); return 0;
    }
    if (g_api->guest_write_u32(g_api, package + 0x94u, count + 1u) != POP_OK) {
        g_api->guest_write_u32(g_api, package + 0x90u, old_array);
        cust_call(0x006a15a0u, 0u, free_new, 1u); return 0;
    }
    /* 6a1560/6a15a0 are the native CRT malloc/free pair. Package destructor
     * 5f7970 frees +90, so it owns the new array after publication. */
    const uint32_t free_old[1] = {old_array};
    cust_call(0x006a15a0u, 0u, free_old, 1u);
    return 1;
}
static int cust_ui_expand(uint32_t package, const char *prefix, uint32_t maximum, int sub) {
    uint32_t count = 0, target = 0;
    for (uint32_t i = 1; i <= maximum; ++i) {
        uint32_t object = cust_ui_find(package, prefix, i);
        if (!object) break;
        count = i; target = object;
    }
    if (!target) return 0;
    uint32_t visited = 0u;
    const uint32_t owner_list = cust_ui_owner_list(package + 0x44u, target, 0u, &visited);
    if (!owner_list) return 0;
    while (count < maximum) {
        const uint32_t clone_args[1] = {0u};
        const uint32_t clone_fn = get_u32(get_u32(target) + 4u);
        const uint32_t clone = cust_call(clone_fn, target, clone_args, 1);
        if (!clone) return 0;
        if (!cust_ui_register_focus(package, target, clone)) {
            const uint32_t destroy[1] = {1u};
            cust_call(get_u32(get_u32(clone)), clone, destroy, 1u);
            log_line("customization pilot: native cloned-control focus registration failed");
            return 0;
        }
        ++count;
        char name[80]; snprintf(name, sizeof(name), "%s%u", prefix, count);
        g_api->guest_write_u32(g_api, clone + 0x10u, cust_hash(name));
        g_api->guest_write_u32(g_api, clone + 0x58u, 0u); /* clear cached render object */
        const uint32_t next = get_u32(target + 4u);
        g_api->guest_write_u32(g_api, clone + 8u, target);
        g_api->guest_write_u32(g_api, clone + 4u, next);
        g_api->guest_write_u32(g_api, target + 4u, clone);
        if (next) g_api->guest_write_u32(g_api, next + 8u, clone);
        if (get_u32(owner_list + 12u) == target)
            g_api->guest_write_u32(g_api, owner_list + 12u, clone);
        g_api->guest_write_u32(g_api, owner_list + 4u, get_u32(owner_list + 4u) + 1u);
        if (get_u32(clone) == 0x009d7654u) {
            uint32_t child = get_u32(clone + 0x64u);
            uint32_t source_child = get_u32(target + 0x64u);
            const uint32_t children = get_u32(clone + 0x60u), last = get_u32(clone + 0x68u);
            char old_name[80];
            snprintf(old_name, sizeof(old_name), "CHECK_MARK_%u", count - 1u);
            const uint32_t old_check = cust_hash(old_name);
            snprintf(old_name, sizeof(old_name), "LOCKED_FABRICATOR_NEW_ICON_%u", count - 1u);
            const uint32_t old_locked = cust_hash(old_name);
            for (uint32_t i = 1; child && source_child && i <= children && i <= 64u; ++i) {
                /* FEGroup::Clone anonymizes child names. Native text, images and
                 * input binding still find those children by their template hashes. */
                const uint32_t source_hash = get_u32(source_child + 0x10u);
                uint32_t child_hash = source_hash;
                if (sub && (source_hash == old_check || source_hash == old_locked)) {
                    snprintf(name, sizeof(name), "%s%u",
                             source_hash == old_check ? "CHECK_MARK_" : "LOCKED_FABRICATOR_NEW_ICON_", count);
                    child_hash = cust_hash(name);
                }
                g_api->guest_write_u32(g_api, child + 0x10u, child_hash);
                if (child == last) break;
                child = get_u32(child + 4u);
                source_child = get_u32(source_child + 4u);
            }
        }
        target = clone;
    }
    return 1;
}
static void cust_ui_package(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t package = cpu->ecx, caller = get_u32(cpu->esp);
    api->call_next(api, inv, cpu);
    if (caller != 0x00600569u || !cust_catalog_ready()) return;
    const char *name = NULL;
    if (api->guest_ptr(api, get_u32(package + 0xcu), 22u, (void **)&name) != POP_OK ||
        !name) return;
    /* Unlimiter _FNGFixes.ini FNG16: the native vinyl category list needs
     * OPTION_1..24. Keep the parts package's separate choice pool unchanged. */
    if (!strcmp(name, "FeVinylSelection.fng")) {
        if (!cust_ui_expand(package, "SUB_OPTION_", 160u, 1) ||
            !cust_ui_expand(package, "OPTION_", 24u, 0)) {
            log_line("customization pilot: required vinyl UI expansion failed"); return;
        }
        cust_call(0x005f3a80u, package, NULL, 0);
        cust_call(0x005f3b70u, package, NULL, 0);
        log_line("customization pilot: vinyl UI supports 24 categories and 160 choices");
        return;
    }
    if(!strcmp(name,"FeCustomizeVisuals.fng")||!strcmp(name,"FeCustomizeMain.fng")){
        if(!cust_ui_expand(package,"OPTION_",12u,0))log_line("customization paint UI expansion unavailable");
        cust_call(0x005f3a80u,package,NULL,0u);cust_call(0x005f3b70u,package,NULL,0u);return;
    }
    if (strcmp(name, "FeCustomizeParts.fng")) return;
    g_cust_ui_visibility_package=package;
    if (!cust_ui_expand(package, "SUB_OPTION_", 160u, 1) ||
        !cust_ui_expand(package, "OPTION_", 32u, 0)) {
        log_line("customization pilot: required parts UI expansion failed"); return;
    }
    cust_ui_visibility_initial(package);
    cust_call(0x005f3a80u, package, NULL, 0);
    cust_call(0x005f3b70u, package, NULL, 0);
    log_line("customization pilot: parts UI supports 32 categories and 160 choices");
}
#define CUST_BT_CAR_ALLOWED(car_id) cust_car_supported(car_id)
/* EC Customization.h137 HandleGenericVinyls. Normal native selectable/cart
 * installs the sentinel first; garage reconciliation removes only generic5E.
 * Record-null preview/cancel follows the source83EEB0 contract. */
static void cust_generic_vinyl_reconcile(uint32_t ride,uint32_t record){
 if(!ride||!cust_catalog_ready()||!cust_pilot_enabled()||!cust_car_supported(get_u32(ride)))return;
 const uint32_t part=get_u32(ride+0x54u+0x5eu*4u);
 if(part&&cust_attr(part,0x03b83203u,0u)){const uint32_t args[3]={ride,record,0x5eu};cust_call(0x0083eeb0u,0u,args,3u);}
}
#include "../brakes-tyres-work/brakes.h"
#include "../brakes-tyres-work/rear-wheels.h"
#include "paint-channel-contract.h"
#include "paint-storage-proposal.h"
#include "paint-menu-proposal.h"
#include "paint-render-proposal.h"
#include "paint-install-proposal.h"
#include "../brakes-tyres-work/tyres.h"
#define CUST_NEON_CAR_SUPPORTED(car_id) cust_car_supported(car_id)
#include "../underglow-work/underglow-render.h"
#include "../underglow-work/underglow-autosculpt.h"
#include "stance-render.h"
#include "stance-autosculpt.h"
#include "stance-install.h"
#include "plate-render.h"
#include "plate-autosculpt.h"
#include "plate-install.h"
#define CUST_CAMERA_DISCRIMINATOR 0
#include "../underglow-work/customization-cameras.h"
#include "../underglow-work/exhaust-center-list.h"
#include "native-tint-vector-support.h"
/* Whole-function wrapper for native GetUsedCarTextureInfo(output, ride).
 * Preserve native output and append required wheel, tyre and decal textures.
 * EC Textures.h adds these dependencies at the same function's epilogue.
 */
static void cust_used_textures(const PopModApi *api, pop_cpu_v1 *cpu,
                               PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t output = get_u32(cpu->esp + 4u);
    const uint32_t ride = get_u32(cpu->esp + 8u);
    const int eligible = output && ride && cust_catalog_ready() && cust_car_supported(get_u32(ride));
    api->call_next(api, inv, cpu);
    if (!eligible) return;
    cust_wheel_used_textures(output,ride);
    cust_tire_used_textures(output,ride);
    for(uint32_t slot=100u;slot<=101u;++slot) {
        const uint32_t part=get_u32(ride+0x54u+slot*4u);
        if(!part || !cust_extension_part(part))continue;
        const uint32_t texture=cust_call(0x007c9eb0u,part,NULL,0);
        uint32_t count=get_u32(output+0x340u),i=0u;
        if(!texture || count>=104u)continue;
        while(i<count && get_u32(output+i*4u)!=texture)++i;
        if(i<count)continue;
        api->guest_write_u32(api,output+count*4u,texture);
        api->guest_write_u32(api,output+0x340u,count+1u);
    }
}
/* Proposed shop policy. Inactive unless explicitly enabled by the private
 * launcher. No progression flags, price, cash, or saved ownership are written. */
static void cust_imported_shop(const PopModApi *api, pop_cpu_v1 *cpu,
                               PopHookInvocation *inv, void *user) {
    (void)user;
    const char *enabled = getenv("RECOMP_CUSTOM_COSMETICS_SHOP");
    const uint32_t trace_caller=get_u32(cpu->esp), trace_part=get_u32(cpu->esp+4u);
    const int trace=g_cust_accept_trace_window && trace_caller==0x00842744u;
    int eligible = 0;
    if (enabled && !strcmp(enabled, "1") && cust_pilot_enabled() && cust_catalog_ready() && cust_supported()) {
        const uint32_t part = get_u32(cpu->esp + 4u);
        eligible = part && cust_extension_part(part);
    }
    const PopModStatus next_status=api->call_next(api, inv, cpu);
    const uint32_t original_eax=cpu->eax;
    if (eligible) cpu->eax = (cpu->eax & 0xffffff00u) | 1u;
    if (trace) {
        char line[320];
        snprintf(line,sizeof line,"PRIVATE accept shop caller=%08x part=%08x index=%u enabled=%s pilot=%d catalog=%d supported=%d eligible=%d next=%d original=%08x final=%08x cart=%u",
                 trace_caller,trace_part,cust_extension_index(trace_part),enabled ? enabled:"unset",
                 cust_pilot_enabled(),cust_catalog_ready(),cust_supported(),eligible,(int)next_status,
                 original_eax,cpu->eax,get_u32(0x00bbaba8u+0x490u));
        log_line(line);
        --g_cust_accept_trace_window;
    }
}
static void cust_neon_screen_title(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)user;const uint32_t screen=cpu->ecx;api->call_next(api,inv,cpu);
 if(cust_neon_as_current()) {const uint32_t args[1]={cust_hash("CUST_PARTS_NEON_TITLE")};cust_call(0x005a3710u,screen,args,1u);}
}
#include "events-profile-init.h"
#include "tyre-resources.h"
#include "driver-entry.h"
static void customization_install(void) {
    if (!cust_pilot_enabled()) return;
    if (g_api->guest_alloc(g_api, 8u, &g_cust_bool_args) != POP_OK) return;
    /* EC Decals.h: rear window uses distinct part group 104. CV/neon uses
     * slot 104 -> group 102, so it does not alias this group. */
    if (get_u32(0x00a73398u + 101u * 4u) != 96u ||
        get_u32(0x00a73398u + 104u * 4u) != 102u) {
        log_line("customization pilot: unexpected decal storage map"); return;
    }
    g_api->guest_write_u32(g_api, 0x00a73398u + 101u * 4u, 104u);
    if(!cust_rear_wheels_map()) return;
    g_api->guest_write_u32(g_api, g_cust_bool_args, 1u);
    g_api->guest_write_u32(g_api, g_cust_bool_args + 4u, 0u);
    const struct { uint32_t address; PopHookFn fn; int mode; uint32_t return_pc; int filtered_only; } hooks[] = {
        {0x006b7620u, cust_tire_after_initialize, POP_HOOK_WRAP},
        {0x007cd930u, cust_decal_texture, POP_HOOK_REPLACE},
        {0x008426a0u, cust_part_price, POP_HOOK_REPLACE},
        {0x007cddf0u, cust_iterator, POP_HOOK_REPLACE},
        {0x0083fae0u, cust_main_setup, POP_HOOK_REPLACE},
        {0x00868390u, cust_main_choice, POP_HOOK_BEFORE},
        {0x00866070u, cust_parts_setup, POP_HOOK_REPLACE},
        {0x0085f9b0u, cust_sub_options, POP_HOOK_REPLACE},
        {0x00852940u, cust_wheel_list, POP_HOOK_REPLACE},
        {0x00842670u, cust_category, POP_HOOK_REPLACE},
        {0x008504d0u, cust_installed_identity, POP_HOOK_REPLACE},
        {0x007c27e0u, cust_record_identity, POP_HOOK_REPLACE},
        {0x00842600u, cust_selectable_identity, POP_HOOK_REPLACE},
        {0x005b2320u, cust_category_option_trace, POP_HOOK_WRAP},
        {0x0083f650u, cust_category_carbon_trace, POP_HOOK_REPLACE},
        {0x004ab0e0u, cust_purecall_trace, POP_HOOK_BEFORE},
        {0x005f7b30u, cust_ui_package, POP_HOOK_REPLACE},
        {0x005c6400u, cust_ui_visibility_reset, POP_HOOK_WRAP},
        {0x005c5ef0u, cust_ui_visibility_finish, POP_HOOK_WRAP},
        {0x007ce880u, cust_used_textures, POP_HOOK_REPLACE},
        {0x004b0b70u, cust_imported_shop, POP_HOOK_WRAP},
        {0x0085b1d0u, cust_neon_screen_title, POP_HOOK_WRAP},
        {0x007d67a0u,cust_rear_set_part,POP_HOOK_WRAP},
        {0x004c47f0u,cust_rear_record_from_ride,POP_HOOK_WRAP},
        {0x0055c140u,cust_rear_brake_marker,POP_HOOK_WRAP},
        {0x008597f0u, cust_bt_garage, POP_HOOK_WRAP},
        {0x008420f0u, cust_bt_selectable_install, POP_HOOK_WRAP},
        {0x007e55e0u, cust_tire_ctor, POP_HOOK_WRAP},
        {0x007d5280u, cust_tire_dtor, POP_HOOK_WRAP},
        {0x007d9e20u, cust_tire_update, POP_HOOK_WRAP},
        {0x007ae210u, cust_tire_wheel3, POP_HOOK_WRAP},
        /* Wheel four and both brake draws share eModel::Render. Keep one
         * broad callback on older hosts; modern hosts filter before snapshots. */
        {0x00729320u, cust_tire_wheel4, POP_HOOK_WRAP, 0x007e0516u, 0},
        {0x00729320u, cust_tire_wheel4, POP_HOOK_WRAP, 0x007e0d06u, 1},
        {0x00729320u, cust_tire_wheel4, POP_HOOK_WRAP, 0x007e0d6cu, 1},
        {0x005ca730u, cust_accept_input_trace, POP_HOOK_BEFORE},
    };
    const int filtered = g_api->size >= offsetof(PopModApi, hook_install_at_callsite) +
        sizeof g_api->hook_install_at_callsite && g_api->hook_install_at_callsite;
    for (uint32_t i = 0; i < sizeof(hooks)/sizeof(hooks[0]); ++i) {
        if (!filtered && hooks[i].filtered_only) continue;
        const PopModStatus status = filtered && hooks[i].return_pc ?
            g_api->hook_install_at_callsite(g_api, hooks[i].address, hooks[i].return_pc,
                hooks[i].fn, hooks[i].mode, NULL, &g_cust_hooks[i]) :
            g_api->hook_install(g_api, hooks[i].address, hooks[i].fn,
                hooks[i].mode, NULL, &g_cust_hooks[i]);
        if (status == POP_OK) continue;
        char failure[128];
        snprintf(failure, sizeof failure, "customization pilot: hook %08x failed status %d",
                 hooks[i].address, (int)status);
        log_line(failure);
        while (i) { --i; if (g_cust_hooks[i]) g_api->hook_remove(g_api, g_cust_hooks[i]); g_cust_hooks[i] = 0; }
        log_line("core.nfsc: customization pilot hooks unavailable"); return;
    }
    if(!cust_paint_hooks_install())log_line("customization independent paint controls unavailable");
    if(!cust_wheel_size_install()) log_line("customization custom wheel size controls unavailable");
    g_cust_neon_render_ready=cust_neon_install();
    if(!g_cust_neon_render_ready) log_line("customization underglow hooks unavailable");
    g_cust_neon_as_ready=g_cust_neon_render_ready && cust_neon_as_install();
    if(!g_cust_neon_as_ready) log_line("customization underglow Autosculpt hooks unavailable");
    g_cust_driver_ready=cust_driver_install();
    g_cust_plate_ready=cust_plate_install();
    g_cust_stance_ready=cust_stance_install();
    if(!g_cust_stance_ready)log_line("customization stance hooks unavailable");
    if(!cust_camera_install()) log_line("customization cameras unavailable");
    if(!cust_exhaust_center_install()) log_line("customization center exhaust attachment support unavailable");
    if(!cust_tv_install()) log_line("customization vector availability support unavailable");
    cust_title_ffn_install();
    /* Challenge Series initialization remains isolated to the private preview.
     * Main and normal named profiles retain their existing progression. */
    {
        const char *profile = getenv("RECOMP_PROFILE_DIR");
        static const char preview_prefix[] = "/opfs/nfsc/profile-customlayout";
        if (profile && !strncmp(profile, preview_prefix, sizeof(preview_prefix) - 1u))
            cust_event_init_install();
    }
    log_line("core.nfsc: isolated supported-road-car customization menus ready");
}

static void customization_shutdown(void) {
    cust_driver_shutdown();
    cust_plate_shutdown();
    cust_stance_shutdown();
    cust_paint_shutdown();
    cust_wheel_size_shutdown();
    cust_tv_shutdown();
}
