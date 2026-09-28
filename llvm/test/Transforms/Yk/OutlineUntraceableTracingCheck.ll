; RUN: opt --passes=yk-module-clone -S < %s | llc --yk-outline-untraceable -print-after=yk-outline-untraceable -o /dev/null > %t 2>&1
; RUN: FileCheck %s --check-prefix=COLD < %t
; RUN: FileCheck %s --check-prefix=HOT < %t

; Check that OutlineUntraceable removes the initial "is tracing?" checks from
; functions that don't need them.

@cold_ptr = global ptr @cold
@hot_ptr = global ptr @hot

; COLD: @cold_ptr = global ptr @cold
; COLD-LABEL: define i32 @cold(i32 %x)
; COLD-NOT: load i8, ptr @__yk_thread_tracing_state
; COLD: add i32 %x, 1
; COLD-NOT: load i8, ptr @__yk_thread_tracing_state
; COLD: ret i32 %result

; HOT: @hot_ptr = global ptr @hot
; HOT-LABEL: define i32 @hot(i32 %x)
; HOT: load i8, ptr @__yk_thread_tracing_state
; HOT: icmp eq i8 {{.*}}, 0

define i32 @cold(i32 %x) {
  %result = add i32 %x, 1
  ret i32 %result
}

define i32 @hot(i32 %x) {
  %result = add i32 %x, 2
  ret i32 %result
}

declare void @yk_mt_control_point(ptr, ptr)

define void @loop(ptr %mt, ptr %loc) {
  call void @yk_mt_control_point(ptr %mt, ptr %loc)
  %result = call i32 @hot(i32 1)
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"wchar_size", i32 4}
