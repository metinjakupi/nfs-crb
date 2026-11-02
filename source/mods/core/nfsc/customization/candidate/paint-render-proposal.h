/* Paint material ownership is per CarRenderInfo, not shared eSolid. */
typedef struct {uint32_t render,ride,storage,valid,native[3],native_carbon,stock_material,stock_cache,stock_native;} CustPaintState;
static CustPaintState g_cust_paints[32];
static CustPaintState*cust_paint_state(uint32_t render){for(uint32_t i=0;i<32u;++i)if(g_cust_paints[i].render==render)return &g_cust_paints[i];return NULL;}
static uint32_t cust_paint_material(CustPaintState*s,uint32_t channel){return s&&(s->valid&(1u<<channel))?s->storage+channel*0x100u+0x10u:0u;}
/* Only the new factory-wheel path owns these optional native pool allocations.
 * Existing aftermarket caches and all native material colors stay untouched. */
static void cust_paint_stock_restore(CustPaintState*s){
 if(!s||!s->stock_material)return;
 if(get_u32(s->render+0x40cu)==s->stock_material)
  g_api->guest_write_u32(g_api,s->render+0x40cu,s->stock_native);
 if(s->stock_cache&&get_u32(s->stock_material)==s->stock_cache){
  /* RideInfo owns the color material. Another live CarRI may borrow it;
   * move our allocation ownership to that registered user before release. */
  CustPaintState*borrower=NULL;
  for(uint32_t i=0;i<32u;++i){CustPaintState*other=&g_cust_paints[i];
   if(other!=s&&other->render&&other->ride==s->ride&&other->stock_material==s->stock_material&&get_u32(other->render+0x40cu)==s->stock_material){borrower=other;break;}
  }
  if(borrower)borrower->stock_cache=s->stock_cache;
  else{cust_call(0x0070e910u,s->stock_material,NULL,0u);g_api->guest_write_u32(g_api,s->stock_material,0u);}
 }
 s->stock_material=0u;s->stock_cache=0u;s->stock_native=0u;
}
static void cust_paint_stock_init(CustPaintState*s){
 if(!s||!s->ride||!cust_bt_allowed(s->ride))return;
 const uint32_t front=cust_bt_part(s->ride,0x54u);
 /* Exact native7E5F24 dirty/paint flag; zero-save does not enter this path. */
 if(!front||!cust_attr(front,0x03b83203u,0u)||!((get_u32(s->ride+0x470u)>>8u)&255u))return;
 const uint32_t material=s->ride+0x380u,cache=get_u32(material);
 if(!cache){cust_call(0x0070e8e0u,material,NULL,0u);if(!get_u32(material))return;s->stock_cache=get_u32(material);}
 cust_call(0x0070e930u,material,NULL,0u);
 s->stock_native=get_u32(s->render+0x40cu);s->stock_material=material;
 g_api->guest_write_u32(g_api,s->render+0x40cu,material);
}
static void cust_paint_restore_ri(CustPaintState*s){
 if(!s||!s->render)return;
 cust_paint_stock_restore(s);
 static const uint32_t offsets[3]={0x410u,0x404u,0x414u},channels[3]={0u,4u,5u};
 const int ownhood=get_u32(s->render+0x404u)==s->storage+4u*0x100u+0x10u;
 for(uint32_t i=0;i<3u;++i)if(get_u32(s->render+offsets[i])==s->storage+channels[i]*0x100u+0x10u)g_api->guest_write_u32(g_api,s->render+offsets[i],s->native[i]);
 if(ownhood)g_api->guest_write_u32(g_api,s->render+0x3ecu,s->native_carbon);
}
static void cust_paint_drop(uint32_t render){
 CustPaintState*s=cust_paint_state(render);if(!s)return;cust_paint_restore_ri(s);
 if(s->storage)g_api->guest_free(g_api,s->storage);
 memset(s,0,sizeof(*s));
}
static void cust_paint_init(uint32_t render,uint32_t ride){
 CustPaintState*s=cust_paint_state(render);
 if(!cust_bt_allowed(ride)){if(s)cust_paint_drop(render);return;}
 if(!s)for(uint32_t i=0;i<32u;++i)if(!g_cust_paints[i].render){s=&g_cust_paints[i];break;}
 if(!s)return;
 if(!s->storage){if(g_api->guest_alloc(g_api,0x600u,&s->storage)!=POP_OK)return;for(uint32_t i=0;i<0x600u;i+=4u)g_api->guest_write_u32(g_api,s->storage+i,0u);}
 /* Native update can retain our previous pointer. Restore before recapturing
  * defaults; newly assigned native pointers are preserved. */
 cust_paint_restore_ri(s);s->render=render;s->ride=ride;s->valid=0u;
 s->native[0]=get_u32(render+0x410u);s->native[1]=get_u32(render+0x404u);s->native[2]=get_u32(render+0x414u);s->native_carbon=get_u32(render+0x3ecu);
 cust_paint_stock_init(s);
 for(uint32_t ch=0;ch<6u;++ch){
  const uint32_t at=cust_paint_ride_color(ride,ch);
  const float a=cust_paint_float_read(at),b=cust_paint_float_read(at+4u),bright=cust_paint_float_read(at+8u),sat=cust_paint_float_read(at+12u);
  if(!(a>0.0f&&a<=40.96f&&b>0.0f&&b<=40.96f&&bright>=0.0f&&bright<=1.0f&&sat>=0.0f&&sat<=1.0f))continue;
  const uint32_t one=cust_paint_by_ordinal((uint32_t)(a*100.0f+0.5f),ch>3u?0x5cu:0x62u),two=cust_paint_by_ordinal((uint32_t)(b*100.0f+0.5f),ch>3u?0x5bu:0x61u);
  if(!one||!two)continue;
  const float v1=1.0f-(float)(uint32_t)(bright*100.0f+0.5f)*0.071428575f,v2=1.0f-(float)(uint32_t)(sat*100.0f+0.5f)*0.11111111f;
  uint32_t f1,f2;memcpy(&f1,&v1,4);memcpy(&f2,&v2,4);const uint32_t args[4]={two,one,f1,f2};
  if(cust_call(0x007d2e60u,s->storage+ch*0x100u,args,4u)&255u)s->valid|=1u<<ch;
 }
 const uint32_t cal=cust_paint_material(s,0u),hood=cust_paint_material(s,4u),spoiler=cust_paint_material(s,5u);
 if(cal)g_api->guest_write_u32(g_api,render+0x410u,cal);
 if(hood){g_api->guest_write_u32(g_api,render+0x404u,hood);g_api->guest_write_u32(g_api,render+0x3ecu,1u);}
 if(spoiler)g_api->guest_write_u32(g_api,render+0x414u,spoiler);
}
typedef struct {uint32_t table,count,values[128];} CustPaintDrawRestore;
static void cust_paint_wheel_begin(uint32_t render,uint32_t wheel,uint32_t model,CustPaintDrawRestore*restore){
 memset(restore,0,sizeof(*restore));CustPaintState*s=cust_paint_state(render);
 if(!s||!model||wheel>3u)return;
 const uint32_t rear=wheel>1u?cust_bt_part(s->ride,0x55u):0u;
 const int rear_default=rear&&cust_attr(rear,0x2c420d64u,0u);
 const uint32_t rim=cust_paint_material(s,wheel<2u||rear_default?1u:2u);
 /* DEFAULT uses the front model. Explicit native front paint follows it;
  * an unpainted chrome/matte model keeps its own original material rows. */
 const uint32_t paint=wheel<2u?0u:rear_default?(((get_u32(s->ride+0x470u)>>8u)&255u)?get_u32(render+0x40cu):0u):cust_paint_material(s,3u);
 if(!rim&&!paint)return;
 const uint32_t solid=get_u32(model+0xcu);if(!solid)return;
 const uint32_t table=get_u32(solid+0x3cu),count=(get_u32(solid+0x18u)>>16u)&255u;
 if(!table||count>127u)return;restore->table=table;restore->count=count;
 for(uint32_t i=0;i<count;++i){
  const uint32_t hash=get_u32(table+i*8u),ptr=get_u32(table+i*8u+4u);restore->values[i]=ptr;
  if(rim&&hash==cust_hash("MAGLIP"))g_api->guest_write_u32(g_api,table+i*8u+4u,rim);
  if(paint&&(hash==cust_hash("MAGSILVER")||hash==cust_hash("MAGCHROME")||hash==cust_hash("MAGMATTE")))g_api->guest_write_u32(g_api,table+i*8u+4u,paint);
 }
}
static void cust_paint_wheel_end(CustPaintDrawRestore*restore){for(uint32_t i=0;i<restore->count;++i)g_api->guest_write_u32(g_api,restore->table+i*8u+4u,restore->values[i]);}
