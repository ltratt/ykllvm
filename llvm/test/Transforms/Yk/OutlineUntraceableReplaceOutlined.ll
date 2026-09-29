; RUN: opt --passes=yk-module-clone -S < %s | llc --yk-outline-untraceable -print-after=yk-outline-untraceable -o /dev/null 2>&1 | FileCheck %s --implicit-check-not='@__yk_opt_cold' --implicit-check-not='@__yk_opt_plain'

; Check that outlined functions are replaced with fully optimised clones.

@cold_ptr = global ptr @cold

; CHECK: @cold_ptr = global ptr @cold
; CHECK-LABEL: define i32 @cold(i32 %x)
; CHECK-NEXT: %result = add i32 %x, 1
; CHECK-NEXT: ret i32 %result
; CHECK-LABEL: define i32 @plain(i32 %x)
; CHECK: mul i32 %x, 3

define i32 @cold(i32 %x) {
  %result = add i32 %x, 1
  ret i32 %result
}

define i32 @plain(i32 %x) {
  %result = mul i32 %x, 3
  ret i32 %result
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"wchar_size", i32 4}
