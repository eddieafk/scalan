; Scala Native runtime resource: lifecycle

define internal void @__scalanative_runtime_startup() {
entry:
  %state = load i8, ptr @__scalanative_runtime_state
  %inactive = icmp eq i8 %state, 0
  br i1 %inactive, label %initialize, label %done
initialize:
  store ptr null, ptr @__scalanative_gc_head
  store i64 0, ptr @__scalanative_gc_allocation_count
  store i64 0, ptr @__scalanative_gc_collection_count
  store i64 64, ptr @__scalanative_gc_collection_threshold
  store ptr null, ptr @__scalanative_shadow_stack
  store ptr null, ptr @__scalanative_source_stack
  store ptr null, ptr @__scalanative_program_arena
  store ptr null, ptr @__scalanative_current_zone
  store ptr null, ptr @__scalanative_exception_handler
  store ptr null, ptr @__scalanative_current_exception
  store i1 false, ptr @__scalanative_reporting_exception
  store i8 1, ptr @__scalanative_runtime_state
  br label %done
done:
  ret void
}

define internal void @__scalanative_runtime_shutdown() {
entry:
  %state = load i8, ptr @__scalanative_runtime_state
  %running = icmp eq i8 %state, 1
  br i1 %running, label %release, label %done
release:
  store i8 2, ptr @__scalanative_runtime_state
  call void @__scalanative_zone_destroy_all()
  call void @__scalanative_gc_collect()
  call void @__scalanative_program_arena_destroy()
  store ptr null, ptr @__scalanative_shadow_stack
  store ptr null, ptr @__scalanative_source_stack
  store ptr null, ptr @__scalanative_current_zone
  store ptr null, ptr @__scalanative_exception_handler
  store ptr null, ptr @__scalanative_current_exception
  store i1 false, ptr @__scalanative_reporting_exception
  br label %done
done:
  ret void
}
