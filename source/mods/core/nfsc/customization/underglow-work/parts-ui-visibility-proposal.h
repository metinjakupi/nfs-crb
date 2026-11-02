/* PROPOSAL ONLY. Include after cust_ui_find, before cust_ui_expand.
 * Integration:
 * 1. After package name check, set g_cust_ui_visibility_package=package.
 * 2. At end of package expansion, before ConnectObjectResources, call
 *    cust_ui_visibility_initial(package).
 * 3. Register 005c6400 -> cust_ui_visibility_reset, POP_HOOK_WRAP.
 * 4. Register 005c5ef0 -> cust_ui_visibility_finish, POP_HOOK_WRAP.
 * These wrappers call original once, preserve its return/register state, and
 * only change parent-group flags. No child icon visibility is changed.
 */
static uint32_t g_cust_ui_visibility_package;
static int cust_ui_visibility_menu(uint32_t menu) {
    const char *name=NULL;
    return cust_catalog_ready() && g_cust_ui_visibility_package && menu &&
        g_api->guest_ptr(g_api,get_u32(menu+0x2c8u),22u,(void**)&name)==POP_OK &&
        name && !strcmp(name,"FeCustomizeParts.fng");
}
static void cust_ui_visibility_group(uint32_t object,int visible) {
    if(!object || get_u32(object)!=0x009d7654u)return;
    uint32_t flags=get_u32(object+0x1cu);
    flags=visible ? ((flags&~1u)|0x02400000u) : (flags|0x02400001u);
    g_api->guest_write_u32(g_api,object+0x1cu,flags);
}
static void cust_ui_visibility_range(uint32_t package,const char *prefix,
                                     uint32_t maximum,uint32_t used) {
    if(used>maximum)used=maximum;
    for(uint32_t i=1;i<=maximum;++i)
        cust_ui_visibility_group(cust_ui_find(package,prefix,i),i<=used);
}
static void cust_ui_visibility_initial(uint32_t package) {
    /* Prevent unused newly cloned groups from drawing before setup finishes. */
    cust_ui_visibility_range(package,"SUB_OPTION_",160u,0u);
    cust_ui_visibility_range(package,"OPTION_",32u,0u);
}
static uint32_t cust_ui_visibility_actual(uint32_t scroller,uint32_t maximum) {
    const uint32_t list=get_u32(scroller+0x114u);
    if(!list)return 0u;
    uint32_t node=get_u32(list), actual=0u, visited=0u;
    while(node && node!=list && visited<4096u) {
        if(node<4u)return 0u;
        if(get_u32(node-4u+0x3cu)!=4u && actual<maximum)++actual;
        node=get_u32(node);
        ++visited;
    }
    if(node!=list)return 0u;
    return actual;
}
static void cust_ui_visibility_apply(uint32_t menu) {
    if(!cust_ui_visibility_menu(menu))return;
    cust_ui_visibility_range(g_cust_ui_visibility_package,"OPTION_",32u,
        cust_ui_visibility_actual(menu+0x28u,32u));
    cust_ui_visibility_range(g_cust_ui_visibility_package,"SUB_OPTION_",160u,
        cust_ui_visibility_actual(menu+0x164u,160u));
}
static void cust_ui_visibility_reset(const PopModApi *api,pop_cpu_v1 *cpu,
                                     PopHookInvocation *inv,void *user) {
    (void)user;uint32_t menu=cpu->ecx;
    api->call_next(api,inv,cpu);
    cust_ui_visibility_apply(menu);
}
static void cust_ui_visibility_finish(const PopModApi *api,pop_cpu_v1 *cpu,
                                      PopHookInvocation *inv,void *user) {
    (void)user;uint32_t menu=cpu->ecx;
    cust_menu_finish_before(cpu);
    api->call_next(api,inv,cpu);
    cust_ui_visibility_apply(menu);
}
