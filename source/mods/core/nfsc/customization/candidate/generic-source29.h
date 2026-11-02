/* Exact actual bundledEC VINYL_GENERIC29 source metadata overlay.
 * Old native identity/typed attrs stay unchanged. GetGenericVinyls source
 * filters CAR_NAME and puts stockNone first. No runtime/asset catalog change. */
typedef struct { uint32_t index,name,label,car; } CustGenericSource;
static const CustGenericSource cust_generic_source[] = {
 {23093u,0x1befa33cu,0xd28bcec5u,0x00000000u}, /* VINYL_GENERIC_NONE: PART_NONE */
 {16030u,0x33b39500u,0xd933bb5cu,0x00000000u}, /* 21STSTREET: PART_VINYL_GENERIC_21STREET */
 {16031u,0xf8fcac00u,0xb57be613u,0x00000000u}, /* BLACKHEARTSGENERIC: PART_VINYL_GENERIC_BLACKHEART */
 {16032u,0xb88eff0du,0x683b41d0u,0x00000000u}, /* BUSHIDO: PART_VINYL_GENERIC_BUSHIDO */
 {16033u,0xd671cd40u,0x861e1003u,0xd671cd40u}, /* CAMARON: PART_VINYL_GENERIC_CAMARON; CAR_NAME=CAMARON */
 {16034u,0xd168c31cu,0x9e84bc57u,0x11565b42u}, /* CECHALLENGER: PART_VINYL_GENERIC_CHALLENGER; CAR_NAME=CHALLENGERN */
 {16035u,0xf87d1da3u,0x95a228beu,0x3b075721u}, /* CECHARGER: PART_VINYL_GENERIC_CHARGER; CAR_NAME=CHARGER06 */
 {16036u,0xae4eae67u,0x5383498au,0xae4eae67u}, /* CHEVELLE: PART_VINYL_GENERIC_CHEVELLE; CAR_NAME=CHEVELLE */
 {16037u,0xfc0dc5b3u,0x7dc244b9u,0x00000000u}, /* COLIBRISGENERIC: PART_VINYL_GENERIC_COLIBRIS */
 {16038u,0xf77c13ebu,0xa72856aeu,0xf77c13ebu}, /* COROLLA: PART_VINYL_GENERIC_COROLLA; CAR_NAME=COROLLA */
 {16039u,0x7502da84u,0x24af1d47u,0x7502da84u}, /* ECLIPSE: PART_VINYL_GENERIC_ECLIPSE; CAR_NAME=ECLIPSE */
 {16040u,0x5a2a2b2bu,0x0a27a18eu,0x5a2a2b2bu}, /* EUROPA: PART_VINYL_GENERIC_EUROPA; CAR_NAME=EUROPA */
 {16041u,0x02de96d2u,0x4e054cd5u,0xcf62f61bu}, /* GT3RS: PART_VINYL_GENERIC_GT3RS; CAR_NAME=997GT3RS */
 {16042u,0x610b85c3u,0x406d14d0u,0x0fcf0041u}, /* HEROCAR: PART_VINYL_GENERIC_HERO; CAR_NAME=BMWM3GTRE46 */
 {16043u,0xc225cfd0u,0xac146524u,0x00000000u}, /* INFERNO: PART_VINYL_GENERIC_INFERNO1 */
 {16044u,0x772966adu,0xac146525u,0x00000000u}, /* INFERNOGENERIC: PART_VINYL_GENERIC_INFERNO2 */
 {16045u,0x030236eeu,0x4e28ecf1u,0x030236eeu}, /* IS300: PART_VINYL_GENERIC_IS300; CAR_NAME=IS300 */
 {16046u,0x32761698u,0x4e48181eu,0x00000000u}, /* KINGSGENERIC: PART_VINYL_GENERIC_KINGS */
 {16047u,0xd107bfc2u,0x55ccd1d4u,0x00000000u}, /* LESAMURAI: PART_VINYL_GENERIC_SAMURAI */
 {16048u,0xed1117ffu,0xb1d907e2u,0xed1117ffu}, /* ROADRUNNER: PART_VINYL_GENERIC_ROADRUNNER; CAR_NAME=ROADRUNNER */
 {16049u,0x9b39d606u,0x280f37acu,0x00000000u}, /* ROTOR4GENERIC: PART_VINYL_GENERIC_ROTOR4 */
 {16050u,0x0899fcd1u,0x67a196a5u,0x00000000u}, /* SCORPIOS: PART_VINYL_GENERIC_SCORPIOS1 */
 {16051u,0xf1cac88eu,0x67a196a6u,0x00000000u}, /* SCORPIOSGENERIC: PART_VINYL_GENERIC_SCORPIOS2 */
 {16052u,0xb5deb714u,0x658af9d7u,0x25964f6eu}, /* SHELBYN: PART_VINYL_GENERIC_SHELBYN; CAR_NAME=MUSTANGSHLBYN */
 {16053u,0x669f0b8cu,0xa8b227f2u,0x00000000u}, /* STACKEDGENERIC1: PART_VINYL_GENERIC_STACKED1 */
 {16054u,0x669f0b8du,0xa8b227f3u,0x00000000u}, /* STACKEDGENERIC2: PART_VINYL_GENERIC_STACKED2 */
 {16055u,0x669f0b8eu,0xa8b227f4u,0x00000000u}, /* STACKEDGENERIC3: PART_VINYL_GENERIC_STACKED3 */
 {16056u,0x669f0b8fu,0xa8b227f5u,0x00000000u}, /* STACKEDGENERIC4: PART_VINYL_GENERIC_STACKED4 */
 {16057u,0x0000e244u,0xe2ec3b87u,0x00000000u}, /* TFK: PART_VINYL_GENERIC_TFK */
};
static uint32_t cust_generic_source_part(uint32_t index){
 for(uint32_t n=get_u32(0x00b76690u),k=0u;n&&n!=0x00b76690u&&k<64u;n=get_u32(n),++k){
  const uint32_t base=get_u32(n+0x3cu),count=get_u32(n+0x38u),parts=get_u32(n+0x34u);
  if(index>=base&&index-base<count)return parts?parts+(index-base)*4u:0u;
 }
 return 0u;
}
static int cust_generic_source_model(uint32_t part){
 const uint32_t table=get_u32(0x00b74cb8u);
 return part&&table&&get_u32(table+((get_u32(part)>>8u)&255u)*4u)==0xaf63534du&&
  ((cust_attr(part,0x9239cf16u,0u)>>8u)&255u)==98u;
}
static uint32_t cust_generic_car_hash(uint32_t car){
 const uint32_t args[1]={car},ptr=cust_call(0x007b0290u,0u,args,1u);if(!ptr)return 0u;
 uint32_t hash=0xffffffffu;
 for(uint32_t i=0u;i<64u;++i){const uint32_t at=ptr+i,ch=(get_u32(at&~3u)>>((at&3u)*8u))&255u;if(!ch)return hash;hash=hash*33u+ch;}
 return 0u;
}
/* Same six-argument7CDDF0 cursor/name/usage contract; first supplied CString
 * arg is0 in sourceGetCarPart. Stock-first supplies source UI AddNodeToListStart. */
static uint32_t cust_generic_source_next(uint32_t car,uint32_t name,uint32_t previous,uint32_t usage){
 if(!cust_pilot_enabled()||!cust_catalog_ready()||!cust_car_supported(car))return 0u;
 uint32_t first=0u;
 if(previous){int found=0;for(uint32_t i=0u;i<29u;++i)if(cust_generic_source_part(cust_generic_source[i].index)==previous){first=i+1u;found=1;break;}if(!found)return 0u;}
 const uint32_t car_hash=cust_generic_car_hash(car);if(!car_hash)return 0u;
 for(uint32_t i=first;i<29u;++i){const CustGenericSource*r=&cust_generic_source[i];
  if((name&&r->name!=name)||(r->car&&r->car!=car_hash))continue;
  const uint32_t part=cust_generic_source_part(r->index);if(!cust_generic_source_model(part))continue;
  if(cust_call(0x007cd930u,part,NULL,0u)!=r->name)continue;
  const uint32_t id=cust_attr(part,0x9239cf16u,0u);
  if(usage!=0xffffffffu&&((id>>5u)&7u)!=usage)continue;
  return part;
 }
 return 0u;
}
/* Native842600 GetName's default API returns LANGUAGEHASH. Preserve the two
 * source-patched unique-identity callers85FA68/85FC36; every other call retains
 * the native display-name contract, including85FB41 list and shopping cart. */
static uint32_t cust_generic_source_label(uint32_t part){
 if(!cust_generic_source_model(part))return 0u;
 const uint32_t name=cust_call(0x007cd930u,part,NULL,0u);
 for(uint32_t i=0u;i<29u;++i)if(cust_generic_source[i].name==name&&cust_generic_source_part(cust_generic_source[i].index)==part)return cust_generic_source[i].label;
 return 0u;
}
