/* Include after paint-menu-proposal.h; own hook or extend paint hook arrays.
 * Whole native83F720 thiscall0/RET0. Native returns bool wheel-IS-STOCK.
 * Preserve original execution and every caller except native Paint wheel gate.
 */
static void cust_paint_stock_menu(const PopModApi*api,pop_cpu_v1*cpu,PopHookInvocation*inv,void*user){
 (void)user;
 const uint32_t caller=get_u32(cpu->esp);
 const int wheel_menu=g_cust_paint_menu==1||(g_cust_paint_menu>=5&&g_cust_paint_menu<=8);
 const int apply=caller==0x00868508u&&cust_paint_enabled()&&g_cust_section==7u&&wheel_menu;
 api->call_next(api,inv,cpu);
 if(apply)cpu->eax=0u;
}
