/* World-map selection and practice races from career free roam. Included after encounter.h. */
#define MR_WORLD 4u
#define MR_CANCEL 8u
static uint32_t g_mr_hooks[9],g_mr_command,g_mr_session,g_mr_custom,g_mr_context,g_mr_event;
static int g_mr_abandoned,g_mr_selecting,g_mr_lobby_loading;
static double g_mr_closed_at,g_mr_prepare_at,g_mr_lobby_closed_at;
static uint32_t g_mr_pending,g_mr_prepare_frames;
static int mr_roam(void) {
 uint32_t rs=race_status();
 return get_u32(RACE_FLOW)==6&&rs&&mp_player()&&!get_u32(rs+0x6a14)&&!get_u32(rs+0x6a1c)&&!mp_online_game()&&!g_cc.session;
}
static void mr_start(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 if(g_mr_selecting||((g_race_page.flags&MR_WORLD)&&get_u32(RACE_FLOW)==6&&!mp_online_game())) {
  const int capture=(g_race_page.flags&(RACE_CATCH|MR_WORLD))==(RACE_CATCH|MR_WORLD)&&mr_roam();
  g_mr_selecting=0;g_mr_closed_at=0;
  uint32_t key=get_u32(cpu->esp+4),type=0,block=race_options(key,&type);RaceShared *sh=race_shared();
  if(capture&&sh&&block) {
   __atomic_fetch_add(&sh->game_seq,1,__ATOMIC_SEQ_CST);
   sh->caught_key=key;sh->caught_type=type;
   for(unsigned i=0;i<40;i++)sh->caught_opts[i]=race_u8(block+i);
   sh->caught++;
   g_mr_lobby_loading=1;g_mr_lobby_closed_at=0;
   __atomic_fetch_add(&sh->game_seq,1,__ATOMIC_RELEASE);
   race_log("core.nfsc: world race: selected %08x type %u without career launch",key,type);
  }
  api->hook_return(api,cpu,0,4);return;
 }
 api->call_original(api,cpu->target,cpu);
}
/* The custom parameters are selected before native Lua records NumRacers.
 * Never change the stock race database record or the player's profile. */
static void mr_activity(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)api;(void)inv;(void)user;
 if(g_mr_session&&g_mr_custom&&g_race_session==g_mr_session&&cpu->ecx==race_status())
  mp_put(cpu->ecx+0x6a1c,g_mr_custom);
}
/* 00644ff0 is the native race-record writer: it writes best time at006451f4
 * and sets career-completed bit2 at006452ab (RET4 at006452e3). Block it
 * separately: callers can reach it without the Lua career/post-race gates.
 * 00642970 is the native career-result transaction (context==3 gate at
 * 0064298b; two thiscall arguments, RET8 at006430f3). FinalizeRaceStats calls
 * it independently of the Lua career predicate. Suppress it for practice.
 * Keep the world lifecycle so Continue returns to free roam. The Lua career
 * predicate and career post-race transaction are disabled only for our custom
 * activity. Cash changes are also blocked for this practice session. */
static void mr_career(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;
 /* The initial native race script caches IsCareerMode as CareerRace. Its
  * race-done closure only uses PostRaceWarpTarget when that cached flag is true.
  * Drift arenas and canyons need this native world setup/return path (without it Enter left
  * both players in the canyon, seen 2026-11-04). This predicate
  * exception never applies to the independent cash/record/transaction hooks. */
 const int drift_world_setup=g_mr_session&&(race_is_drift()||g_race_type==13)&&g_race_phase==RACE_LAUNCHED&&
                            !g_race_active_seen&&cpu->target==0x0064baf0;
 if(g_mr_session&&!drift_world_setup) {api->hook_return(api,cpu,0,(uint32_t)(uintptr_t)user);return;}
 api->call_original(api,cpu->target,cpu);
}
/* Native EnterPostRaceFlow writes campaign completion at0065f229 even when
 * the Lua career predicate is false. Use Quick Race context only inside this
 * synchronous results call, then restore world context for Continue. */
static void mr_postrace(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 if(race_drift_defer_enter()){api->hook_return(api,cpu,0,0);return;}
 uint32_t rs=race_status();
 const int practice=g_mr_session&&rs;
 if(practice)mp_put(rs+0x6a18,0);
 api->call_original(api,cpu->target,cpu);
 if(practice&&rs==race_status())mp_put(rs+0x6a18,3);
}
/* Enter after a race can leave a player where it ended, and the only way out of a canyon is a race:
 * a race without a post-race target (Quick Race events have none) stayed there, in free roam, for
 * good (reported and seen 2026-11-04). So where the race began is kept, and a player back in free
 * roam still this far from it goes to the safehouse by the game's own jump (0064b800, Lua
 * JumpToSafeHouse). A race that does return its players ends well within this. */
#define MR_LEFT_BEHIND 4000.0f
static float g_mr_from[2];
static int g_mr_homing;
static double g_mr_ended_at;
static void mr_keep_start(void) {
 const uint32_t player=mp_player(),state=player?mp_state(get_u32(player+0x70)):0;
 g_mr_homing=0;
 if(!state){g_mr_from[0]=g_mr_from[1]=0;return;}
 g_mr_from[0]=bits_float(get_u32(state+0x10));g_mr_from[1]=bits_float(get_u32(state+0x18));g_mr_homing=-1;
}
static void mr_go_home(void) {
 if(g_mr_homing!=1||mp_now()-g_mr_ended_at<1500)return;
 g_mr_homing=0;
 const uint32_t player=mp_player(),state=player?mp_state(get_u32(player+0x70)):0;
 if(!state||!mr_roam())return;
 const float dx=bits_float(get_u32(state+0x10))-g_mr_from[0],dz=bits_float(get_u32(state+0x18))-g_mr_from[1];
 if(dx*dx+dz*dz<MR_LEFT_BEHIND*MR_LEFT_BEHIND)return;
 race_log("core.nfsc: world race: left %.0f m from its start: to the safehouse",sqrtf(dx*dx+dz*dz));
 mp_call(0x0064b800,0,0,0);
}
static void mr_service(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)api;(void)cpu;(void)inv;(void)user;
 const RacePage *p=&g_race_page;uint32_t rs=race_status();
 /* Jump to Event opens Carbon's loading overlay before calling our captured
  * start. No race activity exists yet to hide it. Clear only that overlay
  * after the map closes and the waiting lobby has returned to free roam. */
 if(g_mr_lobby_loading) {
  if(p->session||g_mr_session||g_mr_pending)g_mr_lobby_loading=0;
  else if(get_u32(0x00a977f0)||!mr_roam())g_mr_lobby_closed_at=0;
  else if(!g_mr_lobby_closed_at)g_mr_lobby_closed_at=mp_now();
  else if(mp_now()-g_mr_lobby_closed_at>=300) {
   mp_call(0x0064ca70,0,0,0); /* original Game.HideLoadingScreen, plain RET */
   g_mr_lobby_loading=0;
  }
 }

 if(g_mr_pending&&(p->session!=g_mr_pending||(p->flags&MR_CANCEL)||!mr_roam())) {
  race_forget("world race preparation cancelled");g_mr_pending=0;return;
 }
 if(g_mr_selecting) {
  if(get_u32(0x00a977f0))g_mr_closed_at=0;
  else if(!g_mr_closed_at)g_mr_closed_at=mp_now();
  else if(mp_now()-g_mr_closed_at>2000)g_mr_selecting=0;
 }
 if(p->spare[0]!=g_mr_command) {
  g_mr_command=p->spare[0];
  if((p->flags&(RACE_CATCH|MR_WORLD))==(RACE_CATCH|MR_WORLD)&&mr_roam()&&!get_u32(0x00a977f0)) {
   g_mr_selecting=1;g_mr_closed_at=0;
   const uint32_t args[]={5,0,0};mp_call(0x005b02b0,get_u32(RACE_FE_DB),args,3);
  }
 }
 if(g_mr_session) {
  if((p->session!=g_mr_session||(p->flags&MR_CANCEL))&&!g_mr_abandoned) {
   g_race_result_activity=g_race_result_simable=0;g_race_aborting=1;g_mr_abandoned=rs&&get_u32(rs+0x6a14)==1u?2:1;
   mp_call(0x0067b600,g_mr_event,0,0);
  }
  if(g_mr_abandoned==1&&rs&&get_u32(rs+0x6a14)==1u) {
   g_mr_abandoned=2;mp_call(0x0067b600,g_mr_event,0,0);
  }
  if(!g_race_session&&((get_u32(RACE_FLOW)==6&&rs&&!get_u32(rs+0x6a14)&&!get_u32(rs+0x6a1c))||get_u32(RACE_FLOW)==3)) {
   if(rs)mp_put(rs+0x6a18,g_mr_context);
   /* Native teardown can still read the custom parameters after clearing the
    * race-status pointer. Keep the database slot alive until its next native
    * replacement, as the original in-world race path does. */
   g_mr_session=g_mr_custom=0;g_mr_abandoned=0;
   if(g_mr_homing<0){g_mr_homing=1;g_mr_ended_at=mp_now();}
  }
  return;
 }
 mr_go_home();
 if(!(p->flags&MR_WORLD)||(p->flags&MR_CANCEL)||!p->session||p->launch!=p->session||
    (p->launch==g_race_launched&&!g_mr_pending)||!mr_roam()||get_u32(0x00a977f0))return;
 /* Kill marks a free-roam car for removal at the next simulation step.
  * Starting InitRacers in that same service call includes the dying vehicle.
  * Reserve the roster now, then let native simulation remove those cars first. */
 if(g_mr_pending!=p->session) {
  race_forget("preparing world race");g_race_session=p->session;g_race_launched=p->session;g_race_phase=RACE_LAUNCHED;
  for(int i=0;i<MP_SLOTS;i++)if(g_mp_remote[i].simable) {
   if(g_mp_friend_car)g_mp_friend_car(i,g_mp_remote[i].id,0);
   mp_drop_friend(&g_mp_remote[i],"world-map race prepares");memset(&g_mp_remote[i],0,sizeof g_mp_remote[i]);
  }
  g_mr_pending=p->session;g_mr_prepare_at=mp_now();g_mr_prepare_frames=0;return;
 }
 if(mp_call(MP_IS_PAUSED,0,0,0)&0xffu){g_mr_prepare_at=mp_now();g_mr_prepare_frames=0;return;}
 if(++g_mr_prepare_frames<3||mp_now()-g_mr_prepare_at<500)return;
 g_mr_pending=0;
 uint32_t type=0;const uint32_t options=race_options(p->launch_key,&type);
 /* Limited to Sprint/Circuit/Speedtrap, native Drift/Canyon Drift and Canyon Duel. */
 if(!options||(type!=0&&type!=1&&type!=5&&type!=11&&type!=12&&type!=13)){race_forget("unsupported world race type");return;}
 uint32_t db=get_u32(0x00a9828c),raw=mp_call(0x0061bef0,db,&p->launch_key,1);
 if(!raw){race_forget("world race parameters unavailable");return;}
 /* The original in-world custom-race path retires its previous custom slot
  * here too (00649ab0). Only do this after confirmed free roam, with no active
  * parameters or crew session. Do not delete the selected base record. */
 const uint32_t retired=get_u32(db+0x2c);
 if(retired) {
  if(retired==raw)return;
  mp_put(db+0x2c,0);mp_call(0x006132c0,0,&retired,1);
 }
 uint32_t custom=mp_call(0x00649170,db,&raw,1);if(!custom){race_forget("world custom allocation failed");return;}
 mp_put(db+0x2c,custom);
 for(unsigned i=0;i<40;i++)g_api->guest_write_u8(g_api,g_race_buf+0xc0+i,p->launch_opts[i]);
 const uint32_t opts[]={custom,g_race_buf+0xc0};mp_call(0x007a9dc0,0,opts,2);
 race_forget("practice race from world map");
 g_mr_session=p->session;g_mr_custom=custom;g_mr_context=3;
 mr_keep_start();
 g_race_type=type;mp_put(rs+0x6a18,3);
 g_race_session=p->session;g_race_launched=p->session;g_race_phase=RACE_LAUNCHED;g_race_no_ai=race_is_drift()||!(p->flags&RACE_KEEP_AI);
 for(int i=0;i<MP_SLOTS;i++)if(g_mp_remote[i].simable) {
  if(g_mp_friend_car)g_mp_friend_car(i,g_mp_remote[i].id,0);
  mp_drop_friend(&g_mp_remote[i],"world-map race starts");memset(&g_mp_remote[i],0,sizeof g_mp_remote[i]);
 }
 /* Use the existing world activity lifecycle. mr_career prevents this custom
  * race from entering the career transaction or awarding career cash. */
 mp_call(0x0063f6c0,custom,0,0);
 mp_call(0x00636d50,get_u32(0x00a98294),0,0);
 const uint32_t activity=get_u32(custom+0x14);
 if(activity) {mp_call(0x0063b4f0,activity,0,0);race_log("core.nfsc: world race: launched session %u custom %08x activity %08x",p->session,custom,activity);}
 else {
  mp_put(rs+0x6a18,g_mr_context);race_forget("practice activity missing");
  if(get_u32(db+0x2c)==custom){mp_put(db+0x2c,0);mp_call(0x006132c0,0,&custom,1);}
  g_mr_session=g_mr_custom=0;
 }
}
static void maprace_install(void) {
 if(g_api->guest_alloc(g_api,64,&g_mr_event)!=POP_OK)return;
 uint32_t addr[]={MP_FE_SERVICE,0x0063d820,0x00629710,0x0064baf0,0x0064b1f0,0x004cd3d0,0x00642970,0x0065ef00,0x00644ff0};
 PopHookFn fn[]={mr_service,mr_start,mr_activity,mr_career,mr_career,mr_career,mr_career,mr_postrace,mr_career};
 int modes[]={POP_HOOK_AFTER,POP_HOOK_REPLACE,POP_HOOK_AFTER,POP_HOOK_REPLACE,POP_HOOK_REPLACE,POP_HOOK_REPLACE,POP_HOOK_REPLACE,POP_HOOK_REPLACE,POP_HOOK_REPLACE};
 for(unsigned i=0;i<9;i++)if(g_api->hook_install(g_api,addr[i],fn[i],modes[i],(void*)(uintptr_t)(i==5||i==8?4:i==6?8:0),g_mr_hooks+i)!=POP_OK)
  {race_log("core.nfsc: world race hook failed %08x",addr[i]);return;}
 RaceShared *sh=race_shared();if(sh)sh->version=g_race_drift_ready?3:2;
}
