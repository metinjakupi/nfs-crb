/* Pinned original95-car catalog. Racer ids0..56; police/traffic ids57+ excluded.
 * Decal flags require verified original compressed geometry keys, not car names alone. */
static uint32_t cust_car_id(void) {
 const uint32_t car=get_u32(0x00b77970u);
 return car && car!=0x00b77970u ? get_u32(car+8u):0xffffffffu;
}
static int cust_car_supported(uint32_t id) { return id<57u; }
static int cust_supported(void) { return cust_car_supported(cust_car_id()); }
static int cust_front_decal_supported(uint32_t id) {
 switch(id) {
 case 1u: case 2u: case 3u: case 4u: case 5u: case 6u: case 7u: case 8u: case 9u: case 10u: case 11u: case 12u: case 13u: case 14u: case 15u: case 16u: case 17u: case 18u: case 19u: case 20u: case 21u: case 22u: case 23u: case 24u: case 25u: case 27u: case 28u: case 32u: case 33u: case 34u: case 35u: case 37u: case 38u: case 39u: case 40u: case 42u: case 43u: case 45u: case 50u: case 51u: return 1;
 default:return 0;
 }
}
static int cust_rear_decal_supported(uint32_t id) {
 switch(id) {
 case 1u: case 2u: case 3u: case 4u: case 5u: case 6u: case 7u: case 8u: case 9u: case 10u: case 11u: case 12u: case 13u: case 14u: case 15u: case 16u: case 17u: case 18u: case 19u: case 20u: case 21u: case 22u: case 23u: case 24u: case 25u: case 27u: case 28u: case 32u: case 33u: case 34u: case 35u: case 37u: case 38u: case 39u: case 40u: case 42u: case 43u: case 45u: case 50u: case 51u: return 1;
 default:return 0;
 }
}
