/* Native drift score tables are separate from GRacerInfo. Only shared drift
 * sessions use this bridge; ordinary career drift keeps its original rules.
 * GRaceDatabase+44 is the drift scorer. ResetScore 6bd970 normally creates
 * three virtual opponents when scorer+8 is set, regardless of physical AI.
 * 6bd0f0 adds a named participant; 6bcfe0 resolves its score record.
 * Banked points are record+c (6bdf37), rank is record+8 (6bd4da).
 */
static int g_race_drift_ready,g_drift_releasing,g_drift_ui_pending,g_drift_enter_pending;
static uint32_t g_drift_component,g_drift_buffer,g_drift_me,g_drift_values[RACE_PEERS];
static uint32_t g_drift_hooks[5];
static int race_is_drift(void) {
 return g_race_session&&!g_race_encounter&&(g_race_type==11||g_race_type==12);
}
static void race_drift_clear(void) {
 g_drift_component=g_drift_me=0;g_drift_ui_pending=g_drift_enter_pending=0;
 memset(g_drift_values,0,sizeof g_drift_values);
}
static uint32_t race_drift_score(void) {
 const uint32_t points=race_is_drift()&&g_drift_me?get_u32(g_drift_me+0xc):0;
 return points>0x7fffffffu?0x7fffffffu:points;
}
static int race_drift_failed(void) {
 if(!race_is_drift()||!g_drift_component)return 0;
 const uint32_t state=get_u32(g_drift_component+0x20);
 return state==1||state==2; /* native PlayerWentOverboard / PlayerWasTooSlow */
}
static uint32_t race_drift_value(uint32_t component,uint32_t key) {
 mp_put(g_drift_buffer,key);
 return mp_call(0x006bcfe0,component+0xd0,&g_drift_buffer,1);
}
static void race_drift_reset(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 if(!race_is_drift()){api->call_original(api,cpu->target,cpu);return;}
 const uint32_t component=cpu->ecx;const uint8_t simulated=race_u8(component+8);
 race_drift_clear();
 g_api->guest_write_u8(g_api,component+8,0); /* no virtual opponent names/scores */
 api->call_original(api,cpu->target,cpu);
 g_api->guest_write_u8(g_api,component+8,simulated);
 g_drift_component=component;
 g_drift_me=race_drift_value(component,get_u32(component+0x24));
 for(uint32_t i=0;i<g_race_page.count&&i<RACE_PEERS;i++) {
  const RacePeer *peer=&g_race_page.peers[i];if(!peer->id)continue;
  const uint32_t name=g_drift_buffer+0x20+i*16;
  for(int k=0;k<16;k++)g_api->guest_write_u8(g_api,name+k,k<15?(uint8_t)peer->name[k]:0);
  /* Type 0 is a real player, type 2 is a simulated drift opponent. */
  const uint32_t args[]={peer->id,name,0};mp_call(0x006bd0f0,component,args,3);
  g_drift_values[i]=race_drift_value(component,get_u32(component+0x30));
 }
 mp_call(0x006bd3e0,component,0,0);
}
static void race_drift_no_simulation(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 if(race_is_drift()){api->hook_return(api,cpu,0,0);return;}
 api->call_original(api,cpu->target,cpu);
}
static void race_drift_sync(uint32_t rs) {
 if(!race_is_drift()||!g_drift_component||!g_drift_me||!rs)return;
 for(uint32_t i=0;i<g_race_page.count&&i<RACE_PEERS;i++)
  if(g_drift_values[i])mp_put(g_drift_values[i]+0xc,g_race_page.peers[i].cp&0x7fffffffu); /* drift: points + high-bit DNF */
 mp_call(0x006bd3e0,g_drift_component,0,0);
 const uint32_t mine=race_drift_score();uint32_t place=1;
 for(uint32_t i=0;i<g_race_page.count&&i<RACE_PEERS;i++)
  if(g_drift_values[i]&&!(g_race_page.peers[i].cp&0x80000000u)&&get_u32(g_drift_values[i]+0xc)>mine)place++;
 if(race_drift_failed())place=g_race_page.count+1;
 mp_put(g_drift_me+8,place-1);
 const uint32_t player=mp_player();const int row=player?race_row_of(rs,player+0x24):-1;
 if(row>=0)mp_put(race_row(rs,(uint32_t)row)+0x34,place);
 for(uint32_t i=0;i<g_race_page.count&&i<RACE_PEERS;i++) {
  if(!g_drift_values[i])continue;
  const uint32_t points=get_u32(g_drift_values[i]+0xc);uint32_t rank=!race_drift_failed()&&mine>points?2:1;
  for(uint32_t j=0;j<g_race_page.count&&j<RACE_PEERS;j++)
   if(i!=j&&g_drift_values[j]&&!(g_race_page.peers[j].cp&0x80000000u)&&get_u32(g_drift_values[j]+0xc)>points)rank++;
  if(g_race_page.peers[i].cp&0x80000000u)rank=g_race_page.count+1;
  mp_put(g_drift_values[i]+8,rank-1);
  for(int k=0;k<g_race_cars_n;k++)if(g_race_cars[k].id==g_race_page.peers[i].id)
   mp_put(race_row(rs,g_race_cars[k].row)+0x34,rank);
 }
}
static int race_drift_waiting(void) {return race_is_drift()&&g_drift_ui_pending;}
static void race_drift_finish_local(uint32_t rs,uint32_t player) {
 if(g_race_aborting)return;
 const int row=player?race_row_of(rs,player+0x24):-1;
 if(row>=0&&!race_u8(race_row(rs,(uint32_t)row)+0x46)) {
  if(g_drift_component)mp_call(0x006be1f0,g_drift_component,0,0);
  mp_call(RACE_FINISH,race_row(rs,(uint32_t)row),0,0);
 }
}
static void race_drift_screen(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 if(race_is_drift()&&g_race_aborting){api->hook_return(api,cpu,0,0);return;}
 if(!race_is_drift()||g_drift_releasing){api->call_original(api,cpu->target,cpu);return;}
 /* A local finish or native slow/overboard stop ends this player's attempt.
  * Never announce a shared winner against Carbon's simulated scores. */
 const uint32_t rs=race_status(),player=mp_player();
 race_drift_finish_local(rs,player);
 g_drift_ui_pending=1;g_race_phase=RACE_DONE;
 api->hook_return(api,cpu,0,0);
}
static int race_drift_defer_enter(void) {
 if(!race_is_drift()||g_race_aborting||g_drift_releasing||!g_drift_ui_pending)return 0;
 g_drift_enter_pending=1;return 1;
}
static void race_drift_results(void) {
 if(!race_is_drift()||g_race_aborting||!g_drift_ui_pending)return;
 for(uint32_t i=0;i<g_race_page.count&&i<RACE_PEERS;i++)
  if(g_race_page.peers[i].id&&!g_race_page.peers[i].done)return;
 const uint32_t rs=race_status(),player=mp_player();const int row=player?race_row_of(rs,player+0x24):-1;
 if(row<0)return;
 race_drift_sync(rs);
 const uint32_t won=!race_drift_failed()&&get_u32(race_row(rs,(uint32_t)row)+0x34)==1;
 g_drift_ui_pending=0;g_drift_releasing=1;
 mp_call(0x0065c6a0,0,&won,1);
 if(g_drift_enter_pending){g_drift_enter_pending=0;mp_call(0x0065ef00,0,0,0);}
 g_drift_releasing=0;
}
/* Native play mode is already racing during the locked countdown.
 * The local GRacerInfo GTimer starts at actual GO. Copied cars must not
 * charge their stationary time or points to this player's drift scorer. */
static void race_drift_update(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 if(race_is_drift()) {
  const uint32_t rs=race_status(),player=mp_player();
  const int mine=player?race_row_of(rs,player+0x24):-1;
  int copied=0;const uint32_t vehicle=get_u32(cpu->esp+12);
  for(int i=0;i<g_race_cars_n;i++)if(g_race_cars[i].id&&g_race_cars[i].car.simable&&
      (vehicle==g_race_cars[i].car.simable||vehicle+0x24==g_race_cars[i].car.simable))copied=1;
  if(g_race_aborting||copied||g_race_phase!=RACE_RACING||g_race_page.go!=g_race_session||
      mine<0||!race_u8(race_row(rs,(uint32_t)mine)+0x188)) {
   api->hook_return(api,cpu,0,20);return; /* native RET 0x14: five arguments */
  }
 }
 api->call_original(api,cpu->target,cpu);
}
static void race_drift_install(void) {
 if(g_api->guest_alloc(g_api,0x100,&g_drift_buffer)!=POP_OK)return;
 const uint32_t addr[]={0x006bd970,0x006bac30,0x006bd570,0x0065c6a0,0x006be570};
 PopHookFn fn[]={race_drift_reset,race_drift_no_simulation,race_drift_no_simulation,race_drift_screen,race_drift_update};
 for(unsigned i=0;i<5;i++)if(g_api->hook_install(g_api,addr[i],fn[i],POP_HOOK_REPLACE,0,g_drift_hooks+i)!=POP_OK) {
  race_log("core.nfsc: shared drift hook failed %08x",addr[i]);return;
 }
 g_race_drift_ready=1;
}
