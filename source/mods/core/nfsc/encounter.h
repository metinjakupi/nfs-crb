/* Online participants in Carbon's original free-roam encounter activity.
 * The page owns invitations and delivery. Native code owns eligibility, the
 * original destination map, roster, activity, standings and cancellation.
 * No arbitrary guest calls or test controls belong in this module. */
#define CC_MAGIC 0x314e4343u
#define CC_MAP_PTR 0x00a97b60u
#define CC_MAP_VT 0x009d2e10u
enum { CC_IDLE, CC_OFFERED, CC_SELECTING, CC_CHOSEN, CC_RUNNING, CC_OVER, CC_DECLINED, CC_FAILED, CC_PROMPT, CC_ACCEPTED };
enum { CC_OPEN=1, CC_LAUNCH, CC_CANCEL, CC_RESET, CC_MAP, CC_RESULT };
typedef struct CcPage {
    uint32_t command, action, peer, session, me, destination, caps_n, caps[16];
    float starts[12]; /* global grid order: position and direction for each car */
    RacePage race;
    uint32_t winner;
    float winner_time;
} CcPage;
typedef struct CcShared {
    uint32_t magic, version, page_seq;
    CcPage page;
    uint8_t padding[0x300-12-sizeof(CcPage)];
    uint32_t game_seq, phase, session, offer_count, offer_to, eligible, destination, error, car;
    float own[6], peer[6];
    float own_finish;
    uint32_t finish_session, result_winner;
} CcShared;
_Static_assert(offsetof(CcShared,game_seq)==0x300 && offsetof(CcShared,own)==0x324 && offsetof(CcShared,own_finish)==0x354 &&
               offsetof(CcPage,race)==140 && offsetof(CcPage,winner)==736 && offsetof(CcShared,result_winner)==0x35c, "encounter.js layout");
static uint32_t g_cc_block, g_cc_event, g_cc_hooks[8], g_cc_command;
static CcPage g_cc_page;
static struct {
    uint32_t phase, session, peer, map, destination, error, offer_count, offer_to;
    int in_init, replaying, cancelling, abandoned_loaded, selector, hud_owned;
    uint32_t hud, activity, race_activity, result_winner, result_params;
    int result_posted;
    float own_finish;
    double began;
} g_cc;
static CcShared *cc_shared(void) {
    void *p=0;
    return g_cc_block && g_api->guest_ptr(g_api,g_cc_block,sizeof(CcShared),&p)==POP_OK?p:0;
}
static void cc_read(void) {
    CcShared *sh=cc_shared(); if(!sh)return;
    uint32_t seq=__atomic_load_n(&sh->page_seq,__ATOMIC_ACQUIRE);if(seq&1)return;
    CcPage p;memcpy(&p,&sh->page,sizeof p);__atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(seq==__atomic_load_n(&sh->page_seq,__ATOMIC_ACQUIRE)&&p.caps_n<=16&&p.race.count<=1)g_cc_page=p;
}
static int cc_page(RacePage *out) {
    cc_read();
    if(!g_cc.session || (g_cc.phase==CC_OVER||g_cc.phase==CC_DECLINED||g_cc.phase==CC_FAILED) || g_cc_page.session!=g_cc.session || g_cc_page.race.session!=g_cc.session ||
       g_cc_page.race.me!=g_cc_page.me || g_cc_page.race.count!=1 ||
       g_cc_page.race.peers[0].id!=g_cc.peer)return 0;
    *out=g_cc_page.race;out->flags=0;return 1;
}
static MpCar *cc_friend(uint32_t id) {
    for(int i=0;id&&i<MP_SLOTS;i++)if(g_mp_remote[i].id==id&&g_mp_remote[i].simable)return &g_mp_remote[i];
    return 0;
}
static int cc_near(uint32_t id) {
    uint32_t player=mp_player();MpCar *peer=cc_friend(id);float a[3],b[3];
    if(!player||!peer||!sr_where(player+0x24,a,0)||!sr_where(peer->simable,b,0))return 0;
    float dx=a[0]-b[0],dz=a[2]-b[2];
    return dx*dx+dz*dz<=SR_REACH*SR_REACH&&fabsf(a[1]-b[1])<=10;
}
static int cc_auto_near(uint32_t id) {
    uint32_t player=mp_player();MpCar *peer=cc_friend(id);float a[3],b[3];
    if(!player||!peer||!sr_where(player+0x24,a,0)||!sr_where(peer->simable,b,0))return 0;
    float dx=a[0]-b[0],dz=a[2]-b[2],d=dx*dx+dz*dz;
    return d>=25.0f&&d<=SR_REACH*SR_REACH&&fabsf(a[1]-b[1])<=10;
}
static int cc_eligible(void) {
    uint32_t player=mp_player();
    return sr_free_roam(player)&&!g_race_session&&!g_pu_police&&!g_pu_mine&&!sr_busted(player)&&
           g_sr.phase==SR_IDLE&&!(mp_call(MP_IS_PAUSED,0,0,0)&0xffu)&&!get_u32(CC_MAP_PTR);
}
static int cc_linked(void) {
    SrShared *sh=sr_shared();return sh&&sh->me&&sh->me==g_cc_page.me;
}
static int cc_has_peer(uint32_t id) {
    for(uint32_t i=0;i<g_cc_page.caps_n;i++)if(g_cc_page.caps[i]==id)return 1;
    return 0;
}
/* The original encounter uses Hud_MenuZoneTrigger plus a generic message.
 * Its activity argument is a native activity ID, not a text pointer. */
static uint32_t cc_hud(void) {
    uint32_t list=get_u32(0x00a9ff5cu),n=get_u32(0x00a9ff64u),player=n&&list?get_u32(list):0,vt=player?get_u32(player):0;
    uint32_t hud=vt?mp_call(get_u32(vt+0x20),player,0,0):0;
    const uint32_t type=0x0040b190u;
    uint32_t ui=hud?mp_call(0x0060cb50u,get_u32(hud+4),&type,1):0;
    return ui&&get_u32(ui)==0x009d6f60u?ui:0;
}
static uint32_t cc_encounter_item(void) {return mp_call(0x00627230u,0,0,0);}
static int cc_ai_active(void) {
    uint32_t item=cc_encounter_item(),state=item?get_u32(item+0x9c):0;
    return state&&(race_u8(state+1)&1u);
}
static int cc_owns_hud(uint32_t ui) {
    return g_cc.hud_owned&&ui&&ui==g_cc.hud&&get_u32(ui+0x10)==g_cc.activity&&get_u32(ui+0x18)==1;
}
static int cc_can_offer(void) {
    if(!cc_eligible())return 0;
    uint32_t ui=cc_hud();return ui&&!get_u32(ui+0x18)&&!cc_ai_active();
}
static void cc_hide_prompt(void) {
    uint32_t ui=cc_hud();
    if(cc_owns_hud(ui)&&!cc_ai_active())mp_call(0x005dc3f0u,ui,0,0);
    g_cc.hud_owned=0;g_cc.hud=g_cc.activity=0;
    if(g_sr_looping==2)sr_unsay();
}
static int cc_show_prompt(void) {
    if(!cc_can_offer())return 0;
    uint32_t ui=cc_hud(),item=cc_encounter_item();if(!ui||!item)return 0;
    const uint32_t args[2]={0x009d7e64u,0};
    uint32_t ref=mp_call(0x0061a920u,item,args,2);if(!ref)return 0;
    uint32_t key=mp_call(0x004613e0u,ref+4,0,0);
    uint32_t record=mp_call(0x0061bf80u,get_u32(0x00a9828cu),&key,1);if(!record)return 0;
    uint32_t activity=mp_call(0x00613890u,record,0,0);if(!activity)return 0;
    g_cc.hud=ui;g_cc.activity=activity;g_cc.hud_owned=1;
    mp_call(0x005dc1f0u,ui,&activity,1);
    const SrStyle note={0x75a0bdb0u,2,2,0.0};
    char name[16];memcpy(name,g_cc_page.race.peers[0].name,sizeof name);name[15]=0;
    sr_say_now(&note,"Rival crew challenge: %s",name[0]?name:"online rival");
    return cc_owns_hud(ui);
}
static void cc_accept(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)inv;(void)user;
    if(g_cc.session&&(g_cc.phase==CC_OFFERED||g_cc.phase==CC_PROMPT)&&cc_owns_hud(cc_hud())&&!cc_ai_active()) {
        /* EEngageEvent has no arguments, RET0. Complete its ordinary header
         * while deferring the map until both human drivers accept. */
        mp_put(cpu->ecx,0x009e2450u);mp_put(cpu->ecx+4,0x10);mp_put(cpu->ecx+8,0);mp_put(cpu->ecx+12,0);
        cc_hide_prompt();g_cc.phase=CC_ACCEPTED;
        sr_say_now(&k_sr_info,"Crew Challenge accepted: waiting for the other driver");
        api->hook_return(api,cpu,cpu->ecx,0);return;
    }
    api->call_original(api,cpu->target,cpu);
}
static struct {uint32_t id;double until;} g_cc_cool[16];
static double g_cc_auto_at;
static int g_cc_offer_ready;
static int cc_cooling(uint32_t id,double now) {
    for(int i=0;i<16;i++)if(g_cc_cool[i].id==id)return now<g_cc_cool[i].until;
    return 0;
}
static void cc_cool(uint32_t id,double now) {
    int at=0;for(int i=0;i<16;i++){if(g_cc_cool[i].id==id){at=i;break;}if(g_cc_cool[i].until<g_cc_cool[at].until)at=i;}
    g_cc_cool[at].id=id;g_cc_cool[at].until=now+120000;
}
static int cc_ignore_flash(uint32_t id,const char *name,double now) {(void)id;(void)name;(void)now;return 1;}
static int cc_offer(uint32_t id,const char *name,double now) {
    cc_read();
    if(g_cc.session || g_cc_page.session || !cc_can_offer())return 1;
    if(!cc_has_peer(id)){sr_say(&k_sr_info,"%s needs to reload to use Crew Challenge",name);return 1;}
    SrShared *sh=sr_shared();if(!sh||!sh->me)return 1;
    static uint32_t serial;
    uint32_t session=((uint32_t)now*2654435761u)^(++serial*0x9e3779b9u)^(sh->me<<16)^id;
    g_cc.session=session?session:1;g_cc.peer=id;g_cc.phase=CC_OFFERED;g_cc.began=now;
    g_cc.offer_to=id;g_cc.offer_count++;g_cc.error=0;
    (void)name;return 1;
}
static int cc_legacy(const SrRec *r,uint32_t player,double now) {
    (void)player;(void)now;
    if(r->k!=SR_CHALLENGE)return 0;
    sr_send_race(r->id,r->r,SR_BUSY,3);return 1;
}
static int cc_owns_map(uint32_t map) {
    return map && map==g_cc.map && get_u32(map)==CC_MAP_VT && get_u32(map+8)==3;
}
static void cc_returned(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)api;(void)inv;(void)user;
    if(g_cc.phase==CC_SELECTING&&cc_owns_map(cpu->ecx)&&get_u32(cpu->esp+4)!=8)g_cc.phase=CC_DECLINED;
}
static void cc_selected(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)inv;(void)user;
    if(!g_cc.replaying&&g_cc.phase==CC_SELECTING&&g_cc.session&&cpu->ecx==get_u32(0x00a982b8u)) {
        g_cc.destination=get_u32(cpu->esp+4);g_cc.phase=CC_CHOSEN;
        api->hook_return(api,cpu,0,4);return;
    }
    api->call_original(api,cpu->target,cpu);
}
static void cc_init(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)inv;(void)user;
    if(g_race_encounter&&g_race_session==g_cc.session)g_cc.in_init=1;
    api->call_original(api,cpu->target,cpu);g_cc.in_init=0;
}
static void cc_register(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)inv;(void)user;
    if(g_cc.in_init) {
        uint32_t player=mp_player(),object=get_u32(cpu->esp+4),vt=object?get_u32(object):0;
        uint32_t sim=vt?mp_call(get_u32(vt+4),object,0,0):0;
        if(player&&sim!=player+0x24){api->hook_return(api,cpu,0,0);return;}
    }
    api->call_original(api,cpu->target,cpu);
}
static void cc_fail(uint32_t error) {
    g_cc.error=error;g_cc.phase=CC_FAILED;
    sr_say_now(&k_sr_info,"Players are no longer ready: Crew Challenge cancelled");
}
static void cc_finished(uint32_t session,float time) {
    /* Called by the native local-placement boundary, after the game measured
     * this car's finish. Retain it until the page has sent the result. */
    if(session==g_cc.session&&g_cc.phase==CC_RUNNING&&!g_cc.cancelling&&!g_cc.own_finish&&isfinite(time)&&time>0)
        g_cc.own_finish=time;
}
/* RaceStatus::SetRaceActivity receives the script's collection wrapper.
 * Retain that local wrapper for the original placement event; guest pointers
 * are never sent across the network. */
static void cc_activity(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)api;(void)inv;(void)user;
    if(g_cc.phase==CC_RUNNING&&g_race_encounter&&g_race_session==g_cc.session&&cpu->ecx==race_status())
        g_cc.race_activity=get_u32(cpu->esp+4);
}
static void cc_result_places(uint32_t rs) {
    if(!g_cc.result_posted||!g_cc.session||get_u32(rs+0x6a1c)!=g_cc.result_params)return;
    uint32_t player=mp_player();int mine=player?race_row_of(rs,player+0x24):-1;
    if(mine<0)return;
    mp_put(race_row(rs,(uint32_t)mine)+0x34,g_cc.result_winner==g_cc_page.me?1:2);
    for(int i=0;i<g_race_cars_n;i++)if(g_race_cars[i].id==g_cc.peer)
        mp_put(race_row(rs,g_race_cars[i].row)+0x34,g_cc.result_winner==g_cc.peer?1:2);
}
static void cc_ranked(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)api;(void)cpu;(void)inv;(void)user;
    const uint32_t rs=race_status();if(rs)cc_result_places(rs);
}
static void cc_result(void) {
    if(g_cc.result_posted||g_cc.cancelling||g_cc.phase!=CC_RUNNING||!g_cc.race_activity||
       !g_race_encounter||g_race_session!=g_cc.session||g_race_phase<RACE_RACING)return;
    const uint32_t winner=g_cc_page.winner,rs=race_status(),player=mp_player();
    const float time=g_cc_page.winner_time;
    if(!rs||!player||!isfinite(time)||time<=0||time>=1200||
       (winner!=g_cc_page.me&&winner!=g_cc.peer))return;
    int mine=race_row_of(rs,player+0x24),row=mine;
    if(winner==g_cc.peer) {
        row=-1;for(int i=0;i<g_race_cars_n;i++)if(g_race_cars[i].id==winner)row=(int)g_race_cars[i].row;
        const RacePeer *peer=race_peer(winner);
        if(!peer||!peer->done||fabsf(peer->time-time)>0.0005f)return;
    } else if(!(g_cc.own_finish>0)||fabsf(g_cc.own_finish-time)>0.0005f)return;
    if(mine<0||row<0||!race_u8(race_row(rs,(uint32_t)row)+0x46)||
       fabsf(bits_float(get_u32(race_row(rs,(uint32_t)row)+0x184))-time)>0.0005f)return;
    /* The host commits the first finish registered by the relay. Native
     * simulation clocks can differ, so that shared decision owns placement.
     * Leave an unfinished loser's measured finish/time untouched. */
    g_cc.result_winner=winner;g_cc.result_params=get_u32(rs+0x6a1c);g_cc.result_posted=1;
    g_race_result_activity=g_race_result_simable=0;cc_result_places(rs);
    const uint32_t place=winner==g_cc_page.me?1:2,args[3]={g_cc.race_activity,player+0x24,place};
    g_race_result_replaying=1;mp_call(RACE_PLACEMENT,0,args,3);g_race_result_replaying=0;
    race_log("core.nfsc: encounter: shared winner %u, local place %u; first finish %.3f s",winner,place,time);
}
static int cc_starts(void) {
    uint32_t player=mp_player();MpCar *peer=cc_friend(g_cc.peer);
    float own[3],other[3];if(!player||!peer||!sr_where(player+0x24,own,0)||!sr_where(peer->simable,other,0))return 0;
    if(g_cc_page.race.grid>1||g_cc_page.race.peers[0].grid!=1-g_cc_page.race.grid)return 0;
    for(int i=0;i<12;i++)if(!isfinite(g_cc_page.starts[i])||fabsf(g_cc_page.starts[i])>100000)return 0;
    for(uint32_t i=0;i<2;i++) {
        const float *s=g_cc_page.starts+i*6;
        float n=s[3]*s[3]+s[4]*s[4]+s[5]*s[5];if(n<0.5f||n>1.5f)return 0;
        const float *p=i==g_cc_page.race.grid?own:other;
        float d=0;for(int k=0;k<3;k++){float x=s[k]-p[k];d+=x*x;}if(d>144)return 0;
    }
    for(uint32_t i=0;i<2;i++) {
        uint32_t grid=i?1-g_cc_page.race.grid:g_cc_page.race.grid;
        for(uint32_t k=0;k<6;k++)put_float(g_cc_event+32+4*k,g_cc_page.starts[grid*6+k]);
        const uint32_t args[3]={i,g_cc_event+32,g_cc_event+44};mp_call(0x006174c0u,0,args,3);
    }
    return 1;
}
static void cc_abort(void) {
    cc_hide_prompt();
    if(g_cc.phase==CC_OFFERED)sr_say_now(&k_sr_info,"Crew Challenge cancelled");
    if(cc_owns_map(get_u32(CC_MAP_PTR))) {
        const uint32_t no=0;mp_call(0x005b2fd0u,g_cc.map,&no,1);
    }
    if(g_race_encounter&&g_race_session==g_cc.session&&!g_cc.cancelling) {
        g_cc.cancelling=1;g_race_aborting=1;g_race_result_activity=g_race_result_simable=0;
        g_cc.abandoned_loaded=g_race_phase>=RACE_RACING;
        /* Original EAbandonRace action: dispatches the activity's abandon event. */
        mp_call(0x0067b600u,g_cc_event,0,0);
    }
    if(!g_race_session)g_cc.phase=CC_DECLINED;
}
static void cc_service(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)api;(void)cpu;(void)inv;(void)user;
    cc_read();CcShared *sh=cc_shared();if(!sh)return;
    if(g_cc_page.command!=g_cc_command) {
        g_cc_command=g_cc_page.command;
        if(g_cc_page.action==CC_OPEN&&!g_cc.session&&cc_linked()&&cc_can_offer()&&cc_has_peer(g_cc_page.peer)&&
           g_cc_page.session&&cc_near(g_cc_page.peer)&&g_cc_page.peer<g_cc_page.me) {
            g_cc.session=g_cc_page.session;g_cc.peer=g_cc_page.peer;g_cc.phase=CC_PROMPT;g_cc.selector=1;g_cc.began=mp_now();
            if(!cc_show_prompt())cc_fail(3);
        } else if(g_cc_page.session==g_cc.session&&g_cc.session) {
            if(g_cc_page.action==CC_MAP&&g_cc.phase==CC_ACCEPTED&&g_cc.selector) {
                if(!cc_linked()||!cc_eligible()||!cc_near(g_cc.peer)||cc_ai_active())cc_fail(4);
                else {
                    cc_hide_prompt();g_cc.phase=CC_SELECTING;
                    mp_call(0x006946d0u,g_cc_event,0,0);mp_call(0x0061d750u,get_u32(0x00a98294u),0,0);
                    g_cc.map=get_u32(CC_MAP_PTR);if(!cc_owns_map(g_cc.map))cc_fail(1);
                }
            } else if(g_cc_page.action==CC_RESULT)cc_result();
            else if(g_cc_page.action==CC_LAUNCH&&(g_cc.phase==CC_ACCEPTED||g_cc.phase==CC_CHOSEN)) {
                if(!cc_linked()||!cc_eligible()||!cc_near(g_cc.peer)||!cc_page(&g_race_page)||
                   g_cc_page.command!=g_cc_command||g_cc_page.action!=CC_LAUNCH||!cc_starts()||
                   !g_cc_page.destination||(g_cc.destination&&g_cc.destination!=g_cc_page.destination))cc_fail(2);
                else {
                    cc_hide_prompt();race_forget("native crew challenge starts");g_race_encounter=1;
                    g_race_session=g_cc.session;g_race_phase=RACE_LAUNCHED;g_race_no_ai=1;g_race_base=0;
                    for(int i=0;i<MP_SLOTS;i++)if(g_mp_remote[i].id==g_cc.peer&&g_mp_remote[i].simable) {
                        if(g_mp_friend_car)g_mp_friend_car(i,g_cc.peer,0);
                        mp_drop_friend(&g_mp_remote[i],"native encounter starts");memset(&g_mp_remote[i],0,sizeof g_mp_remote[i]);
                    }
                    g_cc.replaying=1;g_cc.phase=CC_RUNNING;g_cc.began=mp_now();
                    mp_call(0x00649b80u,get_u32(0x00a982b8u),&g_cc_page.destination,1);g_cc.replaying=0;
                }
            } else if(g_cc_page.action==CC_CANCEL)cc_abort();
            else if(g_cc_page.action==CC_RESET&&(!g_race_encounter||g_race_session!=g_cc.session)&&!cc_owns_map(get_u32(CC_MAP_PTR))) {
                cc_cool(g_cc.peer,mp_now());uint32_t offers=g_cc.offer_count;memset(&g_cc,0,sizeof g_cc);g_cc.offer_count=offers;
            }
        }
    }
    const double now=mp_now();
    if(!g_cc.session&&!g_cc_page.session&&cc_linked()&&now-g_cc_auto_at>250) {
        g_cc_auto_at=now;g_cc_offer_ready=cc_can_offer();
        if(g_cc_offer_ready)for(uint32_t i=0;i<g_cc_page.caps_n;i++) {
            uint32_t id=g_cc_page.caps[i];
            if(id>g_cc_page.me&&!cc_cooling(id,now)&&cc_auto_near(id)){cc_offer(id,"online rival",now);break;}
        }
    }
    if(g_cc.phase==CC_OFFERED&&!g_cc.hud_owned&&g_cc_page.session==g_cc.session&&g_cc_page.peer==g_cc.peer) {
        if(!cc_show_prompt())cc_fail(3);
    }
    if(g_cc.hud_owned&&(g_cc.phase==CC_OFFERED||g_cc.phase==CC_PROMPT)&&!cc_owns_hud(cc_hud()))cc_abort();
    if(g_cc.session&&(g_cc.phase==CC_OFFERED||g_cc.phase==CC_PROMPT||g_cc.phase==CC_ACCEPTED)&&
       (!cc_eligible()||!cc_near(g_cc.peer)||cc_ai_active()))cc_abort();
    if(g_cc.session&&!cc_linked())cc_abort();
    if(g_cc.cancelling&&g_race_encounter&&g_race_session==g_cc.session) {
        uint32_t rs=race_status();
        if(g_race_phase>=RACE_RACING&&!g_cc.abandoned_loaded) {
            /* The original activity cannot receive abandon while its loading
             * wait is blocked. Open that gate without sending network GO,
             * then deliver the same original abandon action once loaded. */
            g_cc.abandoned_loaded=1;mp_call(0x0067b600u,g_cc_event,0,0);
        } else if(g_race_phase==RACE_LAUNCHED&&rs&&!get_u32(rs+0x6a14)&&!get_u32(rs+0x6a1c))
            race_forget("native encounter launch cancelled");
    }
    if(g_cc.phase==CC_RUNNING&&!g_race_session)g_cc.phase=g_cc.cancelling?CC_DECLINED:CC_OVER;
    if(g_cc.phase==CC_RUNNING&&g_race_phase<RACE_RACING&&mp_now()-g_cc.began>60000)cc_abort();
    if(g_cc.session&&(g_cc.phase<=CC_CHOSEN||g_cc.phase==CC_PROMPT||g_cc.phase==CC_ACCEPTED)&&mp_now()-g_cc.began>120000)cc_abort();
    __atomic_fetch_add(&sh->game_seq,1,__ATOMIC_SEQ_CST);
    sh->phase=g_cc.phase;sh->session=g_cc.session;sh->offer_count=g_cc.offer_count;sh->offer_to=g_cc.offer_to;
    sh->eligible=cc_eligible()&&(g_cc.session||g_cc_offer_ready);sh->destination=g_cc.destination;sh->error=g_cc.error;
    sh->own_finish=g_cc.own_finish;sh->finish_session=g_cc.own_finish>0?g_cc.session:0;sh->result_winner=g_cc.result_winner;
    uint32_t player=mp_player();sh->car=player?mp_model_key(player):0;
    memset(sh->own,0,sizeof sh->own);memset(sh->peer,0,sizeof sh->peer);
    if(player)sr_where(player+0x24,sh->own,sh->own+3);
    MpCar *peer=cc_friend(g_cc.peer);if(peer)sr_where(peer->simable,sh->peer,sh->peer+3);
    __atomic_fetch_add(&sh->game_seq,1,__ATOMIC_RELEASE);
}
static void encounter_install(void) {
    g_sr_native_offer=cc_ignore_flash;g_sr_native_receive=cc_legacy;
    if(!g_sr_block||g_api->guest_alloc(g_api,sizeof(CcShared)+64,&g_cc_block)!=POP_OK)return;
    CcShared *sh=cc_shared();memset(sh,0,sizeof *sh);sh->magic=CC_MAGIC;sh->version=3;
    g_cc_event=g_cc_block+sizeof(CcShared);
    uint32_t addr[]={MP_FE_SERVICE,0x005b2fd0u,0x00649b80u,RACE_INIT_RACERS,0x006510c0u,0x00694830u,0x00629710u,RACE_RANK};
    PopHookFn fn[]={cc_service,cc_returned,cc_selected,cc_init,cc_register,cc_accept,cc_activity,cc_ranked};
    /* Normal Online also installs the native sign-in callback. Its BEFORE
     * chain is full, so poll after the frontend service instead. */
    const int mode[]={POP_HOOK_AFTER,POP_HOOK_BEFORE,POP_HOOK_REPLACE,POP_HOOK_REPLACE,POP_HOOK_REPLACE,POP_HOOK_REPLACE,POP_HOOK_BEFORE,POP_HOOK_AFTER};
    for(uint32_t i=0;i<8;i++) {
        PopModStatus status=g_api->hook_install(g_api,addr[i],fn[i],mode[i],0,&g_cc_hooks[i]);
        if(status!=POP_OK) {
            char message[128];snprintf(message,sizeof message,"core.nfsc: encounter: hook %08x failed (%d)",addr[i],(int)status);log_line(message);
            for(uint32_t j=0;j<i;j++)g_api->hook_remove(g_api,g_cc_hooks[j]);return;
        }
    }
    g_race_page_source=cc_page;g_race_finish_observer=cc_finished;g_sr_native_offer=cc_ignore_flash;g_sr_native_receive=cc_legacy;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({var b={};b.memory=wasmMemory;b.ptr=$0;globalThis.carbonEncounterBlock=b;globalThis.dispatchEvent(new Event("carbon-encounter"));},(uint32_t)(uintptr_t)sh);
#endif
}
