/* Native drift score tables are separate from GRacerInfo. Only shared drift
 * sessions use this bridge; ordinary career drift keeps its original rules.
 * GRaceDatabase+44 is the drift scorer. ResetScore 6bd970 normally creates
 * three virtual opponents when scorer+8 is set, regardless of physical AI.
 * 6bd0f0 adds a named participant; 6bcfe0 resolves its score record.
 * Banked points are record+c (6bdf37), rank is record+8 (6bd4da).
 */
static int g_race_drift_ready,g_drift_releasing,g_drift_ui_pending,g_drift_enter_pending;
static uint32_t g_drift_component,g_drift_buffer,g_drift_me,g_drift_values[RACE_PEERS];
static uint32_t g_drift_hooks[12];
static int g_drift_hit_logs; /* log lines of other cars' hits this race (race_drift_hit) */
static uint32_t g_drift_order[RACE_PEERS+1],g_drift_order_n; /* records by rank (race_drift_order) */
static int race_is_drift(void) {
 return g_race_session&&!g_race_encounter&&(g_race_type==11||g_race_type==12);
}
static void race_drift_clear(void) {
 g_drift_component=g_drift_me=0;g_drift_ui_pending=g_drift_enter_pending=0;g_drift_hit_logs=0;
 memset(g_drift_values,0,sizeof g_drift_values);g_drift_order_n=0;
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
/* ONE ORDER FOR EVERY LIST (2026-11-06, logs/drift-rows-20261106). The drift HUD on the right and the
 * results' Standings take each row's name and points from the native rank map (scorer +0xec, rank ->
 * key, rebuilt by 006bd3e0 from points, ties by key), but "you" (row highlight, DAL 0x41a index 0) from
 * the record's rank (+8), and every lookup by position (DAL 0x41a/0x425, 004ca220) from the first racer
 * row whose place (GRacerInfo +0x34) is that position. This file used to write ranks that shared ties
 * and put every DNF on count+1 into +8 and +0x34 only: the three disagreed, two rows could hold the
 * same place and a place could have nobody (seen: "Ben, Cy, Dee, Eve, Eve" with this player missing
 * and the highlight on the second Eve). Now one strict order (finished attempts before DNFs, then
 * points, then relay id: the same in every game) goes into all three, after every native re-rank. */
static void race_drift_order(void) {
 if(!race_is_drift()||!g_drift_component||!g_drift_me)return;
 struct {uint32_t rec,id,points,failed;} e[RACE_PEERS+1],t;uint32_t n=0;
 e[n].rec=g_drift_me;e[n].id=g_race_page.me;e[n].points=get_u32(g_drift_me+0xc);e[n++].failed=(uint32_t)race_drift_failed();
 for(uint32_t i=0;i<g_race_page.count&&i<RACE_PEERS;i++)if(g_drift_values[i]) {
  e[n].rec=g_drift_values[i];e[n].id=g_race_page.peers[i].id;e[n].points=get_u32(g_drift_values[i]+0xc);
  e[n++].failed=g_race_page.peers[i].cp>>31; /* drift: points + high-bit DNF */
 }
 for(uint32_t i=1;i<n;i++)for(uint32_t j=i;j>0;j--) {
  const int later=e[j-1].failed!=e[j].failed?e[j-1].failed>e[j].failed:e[j-1].points!=e[j].points?e[j-1].points<e[j].points:e[j-1].id>e[j].id;
  if(!later)break;
  t=e[j];e[j]=e[j-1];e[j-1]=t;
 }
 const uint32_t rs=race_status(),player=mp_player();const int mine=rs&&player?race_row_of(rs,player+0x24):-1;
 for(uint32_t r=0;r<n;r++) {
  g_drift_order[r]=e[r].rec;
  mp_put(e[r].rec+8,r);
  mp_put(g_drift_buffer+0xf0,r);
  const uint32_t arg=g_drift_buffer+0xf0,key=mp_call(0x006b50c0,g_drift_component+0xec,&arg,1); /* map::operator[] -> &key */
  if(key)mp_put(key,get_u32(e[r].rec+4));
  if(e[r].rec==g_drift_me){if(mine>=0)mp_put(race_row(rs,(uint32_t)mine)+0x34,r+1);}
  else for(int k=0;rs&&k<g_race_cars_n;k++)if(g_race_cars[k].id==e[r].id)mp_put(race_row(rs,g_race_cars[k].row)+0x34,r+1);
 }
 g_drift_order_n=n;
}
static void race_drift_sync(uint32_t rs) {
 if(!race_is_drift()||!g_drift_component||!g_drift_me||!rs)return;
 for(uint32_t i=0;i<g_race_page.count&&i<RACE_PEERS;i++)
  if(g_drift_values[i])mp_put(g_drift_values[i]+0xc,g_race_page.peers[i].cp&0x7fffffffu); /* drift: points + high-bit DNF */
 mp_call(0x006bd3e0,g_drift_component,0,0);
 race_drift_order(); /* also after 006bd3e0 (race_drift_reranked), here in case that call skipped its hooks */
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
  if(g_race_page.peers[i].id&&!(g_race_page.peers[i].done&1u))return;
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
 * The local GRacerInfo GTimer starts at actual GO. DriftUpdate runs from
 * SuspensionRacer, once a tick for every car that has one, and mp_drive.h gives
 * one to other players' cars. Offline that is one car; here each extra car ran
 * the too-slow timer again (14 a second, not 7: TOO SLOW in half the time, 2026-11-04)
 * and put its own speed into this player's score. Argument 3 is the IVehicle
 * (car + 0xb8), so only this player's own car goes through. */
static void race_drift_update(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 if(race_is_drift()) {
  const uint32_t rs=race_status(),player=mp_player();
  const int mine=player?race_row_of(rs,player+0x24):-1;
  const uint32_t vehicle=get_u32(cpu->esp+12);
  if(g_race_aborting||!player||vehicle!=player+0xb8u||g_race_phase!=RACE_RACING||g_race_page.go!=g_race_session||
      mine<0||!race_u8(race_row(rs,(uint32_t)mine)+0x188)) {
   api->hook_return(api,cpu,0,20);return; /* native RET 0x14: five arguments */
  }
 }
 api->call_original(api,cpu->target,cpu);
}
/* HIT WALL. 006a2f00 (thiscall, ECX = the drift scorer, float *size, RET 4) marks the scorer
 * hit (+0x01) when size > 3.5 (0x00a62f10) and its 1 s guard (+0x38) has run out; the next
 * DriftUpdate then ends the drift with "HIT WALL!" (language string b813d3eb) and halves its points
 * (6bdda0). Its one caller is the chassis' collision handler 006f8ba0 (call 006f8c81, return
 * 006f8c86), which every car with a SuspensionRacer runs for its own wall and car hits, and does
 * not ask whose car it is: offline that is only this player's. Here other players' cars have one
 * too (mp_drive.h), so their scrapes and their fall over the edge ended this player's drift
 * (measured 2026-11-05, logs/canyon-drift-20261105). At that call ESI is the handler's object,
 * 0x84 into the chassis, whose owner ISimable is at +0x2c (ESI - 0x58); the player's is
 * player + 0x24. Only this player's own hits go through. */
#define RACE_DRIFT_HIT_RET 0x006f8c86u
static void race_drift_hit(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 if(race_is_drift()&&get_u32(cpu->esp)==RACE_DRIFT_HIT_RET) {
  const uint32_t player=mp_player(),owner=get_u32(cpu->esi-0x58u);
  if(player&&owner!=player+0x24u) {
   const uint32_t bits=get_u32(get_u32(cpu->esp+4));float size;memcpy(&size,&bits,4);
   if(size>3.5f&&g_drift_hit_logs<40) {
    g_drift_hit_logs++;
    race_log("core.nfsc: race: drift: another car's hit (%08x, %.1f) is not ours",owner,size);
   }
   api->hook_return(api,cpu,0,4);return;
  }
 }
 api->call_original(api,cpu->target,cpu);
}
/* IVehicle::SetPhysicsMode 006d6950 (thiscall, ECX = the car + 0xb8, mode, RET 4): mode 0 is
 * neither simulated nor drawn. Deactivate 006c0c00 calls it through the car's table (return
 * address 006c0c07), and is itself reached only through the table, so it has no hook of its own.
 * 00455ca0 deactivates, as the world loads, each of the scene's named cars car1..car30 (0xb4d964's
 * lookup; table 00a840f4) that exists. Only a race with a scripted start has them, and that is
 * Canyon Drift, which offline has one car: a Sprint leaves car 2 alone. Here it is another
 * player's car: its name tag stayed, on a car that was not there (seen 2026-11-04). Nothing else
 * is held back: mp.h and cops.h park cars through this same setter, not through Deactivate. */
#define RACE_DEACTIVATE_RET 0x006c0c07u
static void race_keep_racer(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 const uint32_t mode=get_u32(cpu->esp+4),simable=cpu->ecx-0xb8u+0x24u;
 const int deactivate=get_u32(cpu->esp)==RACE_DEACTIVATE_RET&&mode==0;
 /* the race setup activates every racer: not before go; nor, after it, a car kept off the road; our own calls
    (race start: a loaded player's car on its grid slot before go) pass */
 const int early=mode!=0&&!g_race_switching&&(!g_race_cars_shown||race_kept_off(simable));
 if(mode!=0&&g_race_helper&&simable==g_race_helper){api->hook_return(api,cpu,0,4);return;} /* never */
 if((deactivate||early)&&!g_race_encounter)for(int i=0;i<g_race_cars_n;i++)
  if(g_race_cars[i].id&&g_race_cars[i].car.simable==simable) {
   api->hook_return(api,cpu,0,4);return;
  }
 api->call_original(api,cpu->target,cpu);
}
static void race_at_go(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)api;(void)cpu;(void)inv;(void)user;
 race_drop_helper();
}
/* Every native re-rank (006bd3e0: resets, banked drifts, DriftFinalizeScore) is followed by our order. */
static void race_drift_reranked(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)api;(void)inv;(void)user;
 if(race_is_drift()&&cpu->ecx==g_drift_component)race_drift_order();
}
/* MORE THAN FOUR. Offline drift has four drivers (this player and three simulated ones), so the
 * game's by-rank lookups answer only DAL indexes 2..5 (ranks 0..3): the record 004a6890 (points,
 * DAL 0x42f), the name 004cb1a0 (DAL 0x44f) and the position 004cafc0 (DAL 0x41a). For index 6 and up
 * the name call answered "done" without writing, so the HUD row repeated the row above it, with
 * 0 PTS, and the results' Standings showed an empty row with "---" (measured with five players,
 * 2026-11-06). In a shared drift those indexes are answered from the same order. */
#define DRIFT_RANKED(index) (race_is_drift()&&g_drift_component&&(index)>=6u&&(index)-2u<g_drift_order_n)
static void race_drift_record_at(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 const uint32_t index=get_u32(cpu->esp+4);
 if(DRIFT_RANKED(index)){api->hook_return(api,cpu,g_drift_order[index-2],4);return;} /* thiscall (index), RET 4 */
 api->call_original(api,cpu->target,cpu);
}
static void race_drift_name_at(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 const uint32_t out=get_u32(cpu->esp+4),size=get_u32(cpu->esp+8),index=get_u32(cpu->esp+12);
 if(DRIFT_RANKED(index)&&out&&size) { /* thiscall (char *out, size, index), RET 0xc */
  const uint32_t name=g_drift_order[index-2]+0x38;uint32_t i=0;
  for(;i+1<size&&i<31;i++){const uint8_t ch=race_u8(name+i);if(!ch)break;g_api->guest_write_u8(g_api,out+i,ch);}
  g_api->guest_write_u8(g_api,out+i,0);
  api->hook_return(api,cpu,1,12);return;
 }
 api->call_original(api,cpu->target,cpu);
}
static void race_drift_place_at(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)inv;(void)user;
 const uint32_t out=get_u32(cpu->esp+4),index=get_u32(cpu->esp+8);
 if(DRIFT_RANKED(index)&&out){mp_put(out,index-1);api->hook_return(api,cpu,1,8);return;} /* thiscall (int *out, index), RET 8 */
 api->call_original(api,cpu->target,cpu);
}
static void race_drift_install(void) {
 if(g_api->guest_alloc(g_api,0x100,&g_drift_buffer)!=POP_OK)return;
 const uint32_t addr[]={0x006bd970,0x006bac30,0x006bd570,0x0065c6a0,0x006be570};
 PopHookFn fn[]={race_drift_reset,race_drift_no_simulation,race_drift_no_simulation,race_drift_screen,race_drift_update};
 for(unsigned i=0;i<5;i++)if(g_api->hook_install(g_api,addr[i],fn[i],POP_HOOK_REPLACE,0,g_drift_hooks+i)!=POP_OK) {
  race_log("core.nfsc: shared drift hook failed %08x",addr[i]);return;
 }
 /* Without it Canyon Drift hides the other cars, which is no reason to give up shared drift. */
 if(g_api->hook_install(g_api,0x006d6950,race_keep_racer,POP_HOOK_REPLACE,0,g_drift_hooks+5)!=POP_OK)
  race_log("core.nfsc: shared drift hook failed 006d6950");
 if(g_api->hook_install(g_api,0x006567b0,race_at_go,POP_HOOK_BEFORE,0,g_drift_hooks+6)!=POP_OK)
  race_log("core.nfsc: shared drift hook failed 006567b0");
 if(g_api->hook_install(g_api,0x006a2f00,race_drift_hit,POP_HOOK_REPLACE,0,g_drift_hooks+7)!=POP_OK)
  race_log("core.nfsc: shared drift hook failed 006a2f00");
 /* One order everywhere, and rows past the fourth (see race_drift_order and DRIFT_RANKED). */
 const uint32_t more[]={0x004a6890,0x004cb1a0,0x004cafc0};
 PopHookFn morefn[]={race_drift_record_at,race_drift_name_at,race_drift_place_at};
 if(g_api->hook_install(g_api,0x006bd3e0,race_drift_reranked,POP_HOOK_AFTER,0,g_drift_hooks+8)!=POP_OK)
  race_log("core.nfsc: shared drift hook failed 006bd3e0");
 for(unsigned i=0;i<3;i++)if(g_api->hook_install(g_api,more[i],morefn[i],POP_HOOK_REPLACE,0,g_drift_hooks+9+i)!=POP_OK)
  race_log("core.nfsc: shared drift hook failed %08x",more[i]);
 g_race_drift_ready=1;
}
