; $MODE = "UniformRegister"
; $ATTRIB_VARS[0].name = "position"
; $ATTRIB_VARS[0].type = "float4"
; $ATTRIB_VARS[0].location = 0
; $ATTRIB_VARS[1].name = "tex_coords"
; $ATTRIB_VARS[1].type = "float2"
; $ATTRIB_VARS[1].location = 1
; $ATTRIB_VARS[2].name = "vtx_color"
; $ATTRIB_VARS[2].type = "float4"
; $ATTRIB_VARS[2].location = 2
; $ATTRIB_VARS[3].name = "vtx_attr"
; $ATTRIB_VARS[3].type = "float4"
; $ATTRIB_VARS[3].location = 3
; $SPI_VS_OUT_CONFIG.VS_EXPORT_COUNT = 2
; $NUM_SPI_VS_OUT_ID = 1
; $SPI_VS_OUT_ID[0].SEMANTIC_0 = 0
; $SPI_VS_OUT_ID[0].SEMANTIC_1 = 1
; $SPI_VS_OUT_ID[0].SEMANTIC_2 = 2

00 CALL_FS NO_BARRIER
01 EXP_DONE: POS0, R1
02 EXP: PARAM0, R2 NO_BARRIER
03 EXP: PARAM1, R3 NO_BARRIER
04 EXP_DONE: PARAM2, R4 NO_BARRIER
END_OF_PROGRAM
