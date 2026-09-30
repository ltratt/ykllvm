; RUN: llc -mtriple=x86_64-- -stop-after=yk-stackmaps --yk-insert-stackmaps %s -o - | FileCheck %s

; Lifetime markers do not need to keep values alive, which means that stackmaps
; can then see fewer live variables.

%struct.YkLocation = type { i64 }

define void @callee() {
  ret void
}

define void @lifetime_end() {
; CHECK-LABEL: define void @lifetime_end
; CHECK: call void @callee()
; CHECK-NEXT: call void (i64, i32, ...) @llvm.experimental.stackmap(i64 {{[0-9]+}}, i32 0)
; CHECK-NEXT: call void @llvm.lifetime.end.p0(ptr %slot)
entry:
  %slot = alloca %struct.YkLocation
  call void @escape(ptr %slot)
  call void @callee()
  call void @llvm.lifetime.end.p0(ptr %slot)
  ret void
}

declare void @llvm.lifetime.end.p0(ptr)
declare void @escape(ptr)
