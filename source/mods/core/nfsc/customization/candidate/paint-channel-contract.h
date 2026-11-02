/* Private author-contract helpers. Not integrated; no hooks installed here.
 * FECustomizationRecord existing AS payload has stride44 (no Modified word).
 * RideInfo existing AS payload has stride48. Two16-byte colors per region.
 * Require parent private pilot/catalog/supported gate before any access.
 */
static int cust_paint_channel_from_menu(uint32_t item) {
 switch(item){case 5:return 0;case 6:return 1;case 7:return 2;case 8:return 3;case 9:return 4;case 10:return 5;default:return -1;}
}
static uint32_t cust_paint_slot_from_channel(uint32_t channel) {
 static const uint32_t slots[6]={0x80u,0x82u,0x83u,0x81u,0x84u,0x85u};
 return channel<6u?slots[channel]:0u;
}
static uint32_t cust_paint_saved_color(uint32_t record,uint32_t channel) {
 return record && channel<6u?record+0xccu+(10u+channel/2u)*44u+(channel%2u)*16u:0u;
}
static uint32_t cust_paint_ride_color(uint32_t ride,uint32_t channel) {
 return ride && channel<6u?ride+0x800u+(10u+channel/2u)*48u+(channel%2u)*16u:0u;
}
