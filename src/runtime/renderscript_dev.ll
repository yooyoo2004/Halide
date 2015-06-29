%struct.rs_allocation = type { i32* }

; Function Attrs: nounwind readnone
; declare <4 x float> @rsUnpackColor8888(<4 x i8>) #1

declare <4 x i8> @_Z21rsGetElementAt_uchar413rs_allocationjj([1 x i32], i32, i32)
declare zeroext i8 @_Z20rsGetElementAt_uchar13rs_allocationjjj([1 x i32], i32, i32, i32)

declare <4 x i16> @_Z21rsGetElementAt_short413rs_allocationjj([1 x i32], i32, i32)
declare signext i16 @_Z20rsGetElementAt_short13rs_allocationjj([1 x i32], i32, i32)
declare signext i16 @_Z20rsGetElementAt_short13rs_allocationjjj([1 x i32], i32, i32, i32)

declare <4 x i32> @_Z19rsGetElementAt_int413rs_allocationjj([1 x i32], i32, i32)
declare i32 @_Z18rsGetElementAt_int13rs_allocationjj([1 x i32], i32, i32)
declare i32 @_Z18rsGetElementAt_int13rs_allocationjjj([1 x i32], i32, i32, i32)

declare <4 x float> @_Z21rsGetElementAt_float413rs_allocationjj([1 x i32], i32, i32)
declare float @_Z20rsGetElementAt_float13rs_allocationjjj([1 x i32], i32, i32, i32)
declare float @_Z20rsGetElementAt_float13rs_allocationjj([1 x i32], i32, i32)

; Function Attrs: nounwind readnone
declare float @_Z3dotDv3_fS_(<3 x float>, <3 x float>) #1

; Function Attrs: nounwind readnone
; declare <4 x i8> @_Z17rsPackColorTo8888Dv3_f(<3 x float>) #1

declare void @_Z21rsSetElementAt_uchar413rs_allocationDv4_hjj([1 x i32], <4 x i8>, i32, i32)
declare void @_Z20rsSetElementAt_uchar13rs_allocationhjjj([1 x i32], i8 zeroext, i32, i32, i32)

declare void @_Z21rsSetElementAt_short413rs_allocationDv4_sjj([1 x i32], <4 x i16>, i32, i32)
declare void @_Z20rsSetElementAt_short13rs_allocationsjj([1 x i32], i16 signext, i32, i32)
declare void @_Z20rsSetElementAt_short13rs_allocationsjjj([1 x i32], i16 signext, i32, i32, i32)

declare void @_Z19rsSetElementAt_int413rs_allocationDv4_ijj([1 x i32], <4 x i32>, i32, i32)
declare void @_Z18rsSetElementAt_int13rs_allocationijj([1 x i32], i32, i32, i32)
declare void @_Z18rsSetElementAt_int13rs_allocationijjj([1 x i32], i32, i32, i32, i32)

declare void @_Z21rsSetElementAt_float413rs_allocationDv4_fjj([1 x i32], <4 x float>, i32, i32)
declare void @_Z20rsSetElementAt_float13rs_allocationfjjj([1 x i32], float, i32, i32, i32)
declare void @_Z20rsSetElementAt_float13rs_allocationfjj([1 x i32], float, i32, i32)

; Function Attrs: nounwind
;define void @.rs.dtor() #0 {
;  tail call void @_Z13rsClearObjectP13rs_allocation(%struct.rs_allocation* @alloc_in) #0
;  tail call void @_Z13rsClearObjectP13rs_allocation(%struct.rs_allocation* @alloc_out) #0
;  ret void
;}

declare void @_Z13rsClearObjectP13rs_allocation(%struct.rs_allocation*)

declare void @_Z7rsDebugPKcfff(i8*, float, float, float)