/* PRIVATE proposal: EC CarbonSlot wheel size behavior using whole entries only.
 * Include after cust_menu_finish_before, before cust_parts_setup. Three own
 * handles; no main-array growth. No pool cloning or native record edits here.
 * Runtime normalUI acceptance pending. */
static uint32_t g_cust_wheel_size_hooks[3];
static int g_cust_wheel_size_ready;
static uint32_t cust_wheel_size_filter(uint32_t menu) {
    const uint32_t size=(get_u32(menu+0x2d4u)>>8u)&255u;
    return size>=17u && size<=21u ? size : 17u;
}
static void cust_wheel_size_store(uint32_t menu,uint32_t radius) {
    const uint32_t value=get_u32(menu+0x2d4u);
    g_api->guest_write_u32(g_api,menu+0x2d4u,(value&~0xff00u)|(radius<<8u));
}
/* Call once in custom wheel setup before adding brand categories. Use its
 * returned radius instead of the current fixed17 argument for all brands. */
static uint32_t cust_wheel_size_initial(uint32_t menu,uint32_t slot) {
    uint32_t radius=17u;
    if(!g_cust_wheel_size_ready || !cust_pilot_enabled() || !cust_catalog_ready() ||
       !cust_supported() || !menu || (slot!=0x54u && slot!=0x55u))return radius;
    const uint32_t car=get_u32(0x00b77970u);
    const uint32_t part=car ? get_u32(car+8u+0x54u+slot*4u):0u;
    if(part)radius=cust_attr(part,0xeb0101e2u,17u);
    if(radius<17u || radius>21u)radius=17u;
    cust_wheel_size_store(menu,radius);
    return radius;
}
static uint32_t cust_wheel_size_option(uint32_t menu) {
    if(!g_cust_wheel_size_ready || !cust_pilot_enabled() || !cust_catalog_ready() ||
       !cust_supported() || !menu || (get_u32(menu+0x2d4u)&255u) ||
       (g_cust_section!=5u && g_cust_section!=6u))return 0u;
    const uint32_t option=get_u32(menu+0x34u);
    if(!option)return 0u;
    const uint32_t slot=get_u32(option+0x30u),header=get_u32(option+0x2cu);
    if(slot!=(g_cust_section==6u?0x55u:0x54u) || header==0x0b47ab2eu)return 0u;
    /* Only private registered brands. Original wheel-header path delegates. */
    for(uint32_t i=0;i<g_cust_brand_count && i<64u;++i)
        if(g_cust_brand_headers[i]==header)return option;
    return 0u;
}
/* Invoke at start of existing cust_sub_options wrapper (85F9B0), before
 * call_next. Native866462 hardcodes17 for unknown/custom brand headers on
 * category changes; keep its list radius consistent with WheelFilter. */
static void cust_wheel_size_normalize(pop_cpu_v1 *cpu) {
    const uint32_t menu=cpu->ecx,option=get_u32(cpu->esp+4u);
    if(!option || option!=cust_wheel_size_option(menu) ||
       get_u32(cpu->esp+8u)!=get_u32(option+0x30u) ||
       (get_u32(cpu->esp+16u)&255u))return;
    g_api->guest_write_u32(g_api,cpu->esp+20u,cust_wheel_size_filter(menu));
}
static void cust_wheel_size_prompt(uint32_t menu) {
    const uint32_t package=get_u32(menu+0xcu);
    if(!package)return;
    /* Native840340 uses the same native "%d\"" format at9F79F4. */
    const uint32_t print[4]={package,0xefb07160u,0x009f79f4u,cust_wheel_size_filter(menu)};
    cust_call(0x005a0910u,0u,print,4u);
    const uint32_t label[3]={package,0x25379d08u,0x9248aafeu};
    cust_call(0x005a0830u,0u,label,3u);
}
static void cust_wheel_size_help(uint32_t menu) {
    const uint32_t choice=get_u32(menu+0x170u);
    const uint32_t selectable=choice ? get_u32(choice+0x98u):0u;
    const uint32_t part=selectable ? get_u32(selectable+0x10u):0u;
    const uint32_t message=part && !cust_attr(part,0x03b83203u,0u) ? 0x73dee3bcu:0x53e91f30u;
    const uint32_t engine=get_u32(0x00a97a78u),package=get_u32(menu+0xcu);
    if(engine && package){const uint32_t args[3]={message,package,0u};cust_call(0x00598540u,engine,args,3u);}
}
static void cust_wheel_size_prompt_hook(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;
    if(!cust_wheel_size_option(cpu->ecx)){api->call_next(api,inv,cpu);return;}
    cust_wheel_size_prompt(cpu->ecx);api->hook_return(api,cpu,0u,0u);
}
static void cust_wheel_size_help_hook(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;
    if(!cust_wheel_size_option(cpu->ecx)){api->call_next(api,inv,cpu);return;}
    cust_wheel_size_help(cpu->ecx);api->hook_return(api,cpu,0u,0u);
}
/* Existing pool-visibility helper walks the native sentinel list at
 * scroller+114, excluding ScrollOption subtype4 edge fillers. Native
 * RemoveAll592230 clears count24/28 and current+C; Reset5C6400 rebuilds it.
 * Forward declaration only: reuse the existing validated list walk. */
static uint32_t cust_ui_visibility_actual(uint32_t scroller,uint32_t maximum);
static int cust_wheel_size_populate(uint32_t menu,uint32_t option,uint32_t slot,
                                    uint32_t radius,uint32_t initial) {
    cust_wheel_size_store(menu,radius);
    cust_call(0x00592230u,menu+0x164u,NULL,0u);
    const uint32_t populate[6]={option,slot,0u,0u,radius,initial};
    cust_call(0x0085f9b0u,menu,populate,6u);
    const uint32_t reset[2]={option,1u};
    cust_call(0x005c6400u,menu,reset,2u);
    const uint32_t current=get_u32(menu+0x170u);
    return cust_ui_visibility_actual(menu+0x164u,160u)>0u && current &&
           get_u32(current+0x3cu)!=4u && get_u32(current+0x98u);
}
static void cust_wheel_size_message(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;
    const uint32_t menu=cpu->ecx,event=get_u32(cpu->esp+4u);
    const uint32_t option=event==0xc519bfc3u && (get_u32(menu+0x2e0u)&255u) ? cust_wheel_size_option(menu):0u;
    /* The custom header is not in native86653C's allowlist. Native therefore
     * performs base5CA730 dispatch once but cannot also cycle this category.
     * Revalidate afterwards before the author wheel-specific continuation. */
    const PopModStatus status=api->call_next(api,inv,cpu);
    if(status!=POP_OK || !option || cust_wheel_size_option(menu)!=option)return;
    const uint32_t selected=get_u32(menu+0x170u);
    const uint32_t selectable=selected ? get_u32(selected+0x98u):0u;
    const uint32_t vtable=selectable ? get_u32(selectable):0u;
    const uint32_t guard=vtable ? get_u32(vtable+0x34u):0u;
    uint32_t blocked=1u;
    if(!guard || api->guest_call(api,guard,selectable,NULL,0u,&blocked)!=POP_OK || (blocked&255u))return;
    const uint32_t slot=get_u32(option+0x30u);
    const uint32_t initial=cust_call(0x00591250u,menu+0x164u,NULL,0u);
    const uint32_t old=cust_wheel_size_filter(menu),radius=old==21u?17u:old+1u;
    int populated=cust_wheel_size_populate(menu,option,slot,radius,initial);
    if(!populated) {
        /* One attempt only. A sparse brand/radius must not strand the user in
         * an empty list where native's selected-part guard blocks recovery. */
        populated=cust_wheel_size_populate(menu,option,slot,old,initial);
        if(!populated)log_line("customization wheel size: previous choice list could not be restored");
    }
    cust_call(0x005a4ee0u,menu,NULL,0u);
    cust_wheel_size_prompt(menu);
    cust_wheel_size_help(menu);
    if(!populated)return;
    const uint32_t current=get_u32(menu+0x170u),vt=current ? get_u32(current):0u;
    const uint32_t notify=vt ? get_u32(vt+8u):0u;
    if(notify){const uint32_t args[5]={get_u32(menu+0xcu),0x5b481ed5u,0u,0u,0u};cust_call(notify,current,args,5u);}
}
static void cust_wheel_size_shutdown(void) {
    g_cust_wheel_size_ready=0;
    for(uint32_t i=0;i<3u;++i)if(g_cust_wheel_size_hooks[i]){g_api->hook_remove(g_api,g_cust_wheel_size_hooks[i]);g_cust_wheel_size_hooks[i]=0u;}
}
static int cust_wheel_size_install(void) {
    if(!cust_pilot_enabled())return 0;
    const struct {uint32_t address;PopHookFn callback;} hooks[3]={
        {0x008662c0u,cust_wheel_size_message},
        {0x00840340u,cust_wheel_size_prompt_hook},
        {0x008401f0u,cust_wheel_size_help_hook}
    };
    for(uint32_t i=0;i<3u;++i)if(g_api->hook_install(g_api,hooks[i].address,hooks[i].callback,POP_HOOK_WRAP,NULL,&g_cust_wheel_size_hooks[i])!=POP_OK){cust_wheel_size_shutdown();return 0;}
    g_cust_wheel_size_ready=1;return 1;
}
