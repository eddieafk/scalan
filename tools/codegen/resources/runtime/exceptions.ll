; Scala Native runtime resource: exceptions

@.fmt.uncaught_exception = private unnamed_addr constant [24 x i8] c"Uncaught exception: %s\0A\00"
@.fmt.exception = private unnamed_addr constant [4 x i8] c"%s\0A\00"
@.fmt.caused_by = private unnamed_addr constant [15 x i8] c"Caused by: %s\0A\00"
@.fmt.suppressed = private unnamed_addr constant [16 x i8] c"Suppressed: %s\0A\00"
@.fmt.source_frame = private unnamed_addr constant [15 x i8] c"\09at %s(%s:%u)\0A\00"
@.str.circular_cause = private unnamed_addr constant [21 x i8] c"[circular reference]\00"

define internal ptr @__scalanative_exception_trace(ptr %exception) {
entry:
  %slot = call ptr @__scalanative_exception_trace_slot(ptr %exception)
  %has_slot = icmp ne ptr %slot, null
  br i1 %has_slot, label %load, label %none
load:
  %trace = load ptr, ptr %slot
  ret ptr %trace
none:
  ret ptr null
}

define internal void @__scalanative_capture_exception_trace(ptr %exception) {
entry:
  %slot = call ptr @__scalanative_exception_trace_slot(ptr %exception)
  %has_slot = icmp ne ptr %slot, null
  br i1 %has_slot, label %check_existing, label %done
check_existing:
  %existing = load ptr, ptr %slot
  %is_empty = icmp eq ptr %existing, null
  br i1 %is_empty, label %check_source, label %done
check_source:
  %head = load ptr, ptr @__scalanative_source_stack
  %has_source = icmp ne ptr %head, null
  br i1 %has_source, label %allocate, label %done
allocate:
  %trace_end = getelementptr %scalanative.exception_trace, ptr null, i32 1
  %trace_size = ptrtoint ptr %trace_end to i64
  %trace = call ptr @malloc(i64 %trace_size)
  %allocated = icmp ne ptr %trace, null
  br i1 %allocated, label %copy_start, label %done
copy_start:
  br label %copy
copy:
  %frame = phi ptr [ %head, %copy_start ], [ %previous, %copy_frame ]
  %count = phi i32 [ 0, %copy_start ], [ %next_count, %copy_frame ]
  %has_frame = icmp ne ptr %frame, null
  %has_capacity = icmp ult i32 %count, 64
  %continue = and i1 %has_frame, %has_capacity
  br i1 %continue, label %copy_frame, label %finish
copy_frame:
  %function_field = getelementptr %scalanative.source_frame, ptr %frame, i32 0, i32 1
  %function = load ptr, ptr %function_field
  %file_field = getelementptr %scalanative.source_frame, ptr %frame, i32 0, i32 2
  %file = load ptr, ptr %file_field
  %line_field = getelementptr %scalanative.source_frame, ptr %frame, i32 0, i32 3
  %line = load i32, ptr %line_field
  %column_field = getelementptr %scalanative.source_frame, ptr %frame, i32 0, i32 4
  %column = load i32, ptr %column_field
  %entries = getelementptr %scalanative.exception_trace, ptr %trace, i32 0, i32 1
  %entry_slot = getelementptr [64 x %scalanative.exception_trace_entry], ptr %entries, i32 0, i32 %count
  %entry_function = getelementptr %scalanative.exception_trace_entry, ptr %entry_slot, i32 0, i32 0
  store ptr %function, ptr %entry_function
  %entry_file = getelementptr %scalanative.exception_trace_entry, ptr %entry_slot, i32 0, i32 1
  store ptr %file, ptr %entry_file
  %entry_line = getelementptr %scalanative.exception_trace_entry, ptr %entry_slot, i32 0, i32 2
  store i32 %line, ptr %entry_line
  %entry_column = getelementptr %scalanative.exception_trace_entry, ptr %entry_slot, i32 0, i32 3
  store i32 %column, ptr %entry_column
  %previous_field = getelementptr %scalanative.source_frame, ptr %frame, i32 0, i32 0
  %previous = load ptr, ptr %previous_field
  %next_count = add i32 %count, 1
  br label %copy
finish:
  %count_field = getelementptr %scalanative.exception_trace, ptr %trace, i32 0, i32 0
  store i32 %count, ptr %count_field
  store ptr %trace, ptr %slot
  br label %done
done:
  ret void
}

define internal void @__scalanative_release_exception_trace(ptr %exception) {
entry:
  %slot = call ptr @__scalanative_exception_trace_slot(ptr %exception)
  %has_slot = icmp ne ptr %slot, null
  br i1 %has_slot, label %load, label %done
load:
  %trace = load ptr, ptr %slot
  %has_trace = icmp ne ptr %trace, null
  br i1 %has_trace, label %release, label %done
release:
  call void @free(ptr %trace)
  store ptr null, ptr %slot
  br label %done
done:
  ret void
}

define internal ptr @__scalanative_fill_in_stack_trace(ptr %exception) {
entry:
  call void @__scalanative_release_exception_trace(ptr %exception)
  call void @__scalanative_capture_exception_trace(ptr %exception)
  ret ptr %exception
}

define internal void @__scalanative_report_retained_trace(ptr %trace) {
entry:
  %count_field = getelementptr %scalanative.exception_trace, ptr %trace, i32 0, i32 0
  %count = load i32, ptr %count_field
  br label %frames
frames:
  %index = phi i32 [ 0, %entry ], [ %next_index, %print ]
  %in_bounds = icmp ult i32 %index, %count
  %has_capacity = icmp ult i32 %index, 64
  %continue = and i1 %in_bounds, %has_capacity
  br i1 %continue, label %print, label %done
print:
  %entries = getelementptr %scalanative.exception_trace, ptr %trace, i32 0, i32 1
  %entry_slot = getelementptr [64 x %scalanative.exception_trace_entry], ptr %entries, i32 0, i32 %index
  %function_field = getelementptr %scalanative.exception_trace_entry, ptr %entry_slot, i32 0, i32 0
  %stored_function = load ptr, ptr %function_field
  %function_is_null = icmp eq ptr %stored_function, null
  %function = select i1 %function_is_null, ptr getelementptr inbounds ([10 x i8], ptr @.str.stack_trace_unknown, i64 0, i64 0), ptr %stored_function
  %file_field = getelementptr %scalanative.exception_trace_entry, ptr %entry_slot, i32 0, i32 1
  %stored_file = load ptr, ptr %file_field
  %file_is_null = icmp eq ptr %stored_file, null
  %file = select i1 %file_is_null, ptr getelementptr inbounds ([10 x i8], ptr @.str.stack_trace_unknown, i64 0, i64 0), ptr %stored_file
  %line_field = getelementptr %scalanative.exception_trace_entry, ptr %entry_slot, i32 0, i32 2
  %line = load i32, ptr %line_field
  %error_stream = load ptr, ptr @stderr
  %reported = call i32 (ptr, ptr, ...) @fprintf(ptr %error_stream, ptr getelementptr inbounds ([15 x i8], ptr @.fmt.source_frame, i64 0, i64 0), ptr %function, ptr %file, i32 %line)
  %next_index = add i32 %index, 1
  br label %frames
done:
  ret void
}

define internal void @__scalanative_report_active_source_stack() {
entry:
  %head = load ptr, ptr @__scalanative_source_stack
  br label %frames
frames:
  %frame = phi ptr [ %head, %entry ], [ %previous, %print ]
  %count = phi i32 [ 0, %entry ], [ %next_count, %print ]
  %has_frame = icmp ne ptr %frame, null
  %has_capacity = icmp ult i32 %count, 64
  %continue = and i1 %has_frame, %has_capacity
  br i1 %continue, label %print, label %done
print:
  %function_field = getelementptr %scalanative.source_frame, ptr %frame, i32 0, i32 1
  %function = load ptr, ptr %function_field
  %file_field = getelementptr %scalanative.source_frame, ptr %frame, i32 0, i32 2
  %file = load ptr, ptr %file_field
  %line_field = getelementptr %scalanative.source_frame, ptr %frame, i32 0, i32 3
  %line = load i32, ptr %line_field
  %error_stream = load ptr, ptr @stderr
  %reported = call i32 (ptr, ptr, ...) @fprintf(ptr %error_stream, ptr getelementptr inbounds ([15 x i8], ptr @.fmt.source_frame, i64 0, i64 0), ptr %function, ptr %file, i32 %line)
  %previous_field = getelementptr %scalanative.source_frame, ptr %frame, i32 0, i32 0
  %previous = load ptr, ptr %previous_field
  %next_count = add i32 %count, 1
  br label %frames
done:
  ret void
}

define internal void @__scalanative_report_exception_trace(ptr %exception, i1 %fallback_to_active) {
entry:
  %trace = call ptr @__scalanative_exception_trace(ptr %exception)
  %has_trace = icmp ne ptr %trace, null
  br i1 %has_trace, label %retained, label %fallback
retained:
  call void @__scalanative_report_retained_trace(ptr %trace)
  br label %done
fallback:
  br i1 %fallback_to_active, label %active, label %done
active:
  call void @__scalanative_report_active_source_stack()
  br label %done
done:
  ret void
}

define internal void @__scalanative_report_exception(ptr %exception, i1 %uncaught) {
entry:
  %reporting = load i1, ptr @__scalanative_reporting_exception
  %has_exception = icmp ne ptr %exception, null
  %not_reporting = xor i1 %reporting, true
  %public_report = xor i1 %uncaught, true
  %can_describe = or i1 %not_reporting, %public_report
  %use_dynamic = and i1 %has_exception, %can_describe
  br i1 %use_dynamic, label %dynamic, label %fallback
dynamic:
  br i1 %uncaught, label %mark_reporting, label %describe_dynamic
mark_reporting:
  store i1 true, ptr @__scalanative_reporting_exception
  br label %describe_dynamic
describe_dynamic:
  %dynamic_description = call ptr @__scalanative_any_to_string(ptr %exception)
  br label %print
fallback:
  br i1 %has_exception, label %load_type_name, label %null_value
load_type_name:
  %descriptor = call ptr @__scalanative_object_descriptor(ptr %exception)
  %fallback_type_name = call ptr @__scalanative_descriptor_type_name(ptr %descriptor)
  br label %print
null_value:
  br label %print
print:
  %description = phi ptr [ %dynamic_description, %describe_dynamic ], [ %fallback_type_name, %load_type_name ], [ getelementptr inbounds ([5 x i8], ptr @.str.null, i64 0, i64 0), %null_value ]
  %error_stream = load ptr, ptr @stderr
  %format = select i1 %uncaught, ptr getelementptr inbounds ([24 x i8], ptr @.fmt.uncaught_exception, i64 0, i64 0), ptr getelementptr inbounds ([4 x i8], ptr @.fmt.exception, i64 0, i64 0)
  %reported = call i32 (ptr, ptr, ...) @fprintf(ptr %error_stream, ptr %format, ptr %description)
  call void @__scalanative_report_exception_trace(ptr %exception, i1 true)
  br i1 %use_dynamic, label %causes_setup, label %done
causes_setup:
  call void @__scalanative_report_suppressed_exceptions(ptr %exception)
  %seen = alloca [64 x ptr], align 8
  %root_slot = getelementptr [64 x ptr], ptr %seen, i64 0, i64 0
  store ptr %exception, ptr %root_slot
  %first_cause = call ptr @__scalanative_exception_cause(ptr %exception)
  br label %cause_loop
cause_loop:
  %cause = phi ptr [ %first_cause, %causes_setup ], [ %next_cause, %cause_unique ]
  %seen_count = phi i32 [ 1, %causes_setup ], [ %next_seen_count, %cause_unique ]
  %has_cause = icmp ne ptr %cause, null
  %has_capacity = icmp ult i32 %seen_count, 64
  %continue_causes = and i1 %has_cause, %has_capacity
  br i1 %continue_causes, label %cause_scan_start, label %done
cause_scan_start:
  br label %cause_scan
cause_scan:
  %scan_index = phi i32 [ 0, %cause_scan_start ], [ %next_scan_index, %cause_scan_advance ]
  %scan_complete = icmp uge i32 %scan_index, %seen_count
  br i1 %scan_complete, label %cause_unique, label %cause_scan_value
cause_scan_value:
  %wide_scan_index = zext i32 %scan_index to i64
  %seen_slot = getelementptr [64 x ptr], ptr %seen, i64 0, i64 %wide_scan_index
  %seen_cause = load ptr, ptr %seen_slot
  %duplicate = icmp eq ptr %seen_cause, %cause
  br i1 %duplicate, label %cause_circular, label %cause_scan_advance
cause_scan_advance:
  %next_scan_index = add i32 %scan_index, 1
  br label %cause_scan
cause_unique:
  %wide_seen_count = zext i32 %seen_count to i64
  %next_seen_slot = getelementptr [64 x ptr], ptr %seen, i64 0, i64 %wide_seen_count
  store ptr %cause, ptr %next_seen_slot
  %cause_description = call ptr @__scalanative_any_to_string(ptr %cause)
  %cause_reported = call i32 (ptr, ptr, ...) @fprintf(ptr %error_stream, ptr getelementptr inbounds ([15 x i8], ptr @.fmt.caused_by, i64 0, i64 0), ptr %cause_description)
  call void @__scalanative_report_exception_trace(ptr %cause, i1 false)
  call void @__scalanative_report_suppressed_exceptions(ptr %cause)
  %next_cause = call ptr @__scalanative_exception_cause(ptr %cause)
  %next_seen_count = add i32 %seen_count, 1
  br label %cause_loop
cause_circular:
  %circular_reported = call i32 (ptr, ptr, ...) @fprintf(ptr %error_stream, ptr getelementptr inbounds ([15 x i8], ptr @.fmt.caused_by, i64 0, i64 0), ptr getelementptr inbounds ([21 x i8], ptr @.str.circular_cause, i64 0, i64 0))
  br label %done
done:
  br i1 %uncaught, label %clear_reporting, label %finish
clear_reporting:
  store i1 false, ptr @__scalanative_reporting_exception
  br label %finish
finish:
  ret void
}

define internal void @__scalanative_report_uncaught_exception(ptr %exception) {
entry:
  call void @__scalanative_report_exception(ptr %exception, i1 true)
  ret void
}

define internal void @__scalanative_print_stack_trace(ptr %exception) {
entry:
  call void @__scalanative_report_exception(ptr %exception, i1 false)
  ret void
}

define internal void @__scalanative_throw(ptr %exception) noreturn {
entry:
  call void @__scalanative_capture_exception_trace(ptr %exception)
  %handler = load ptr, ptr @__scalanative_exception_handler
  %handled = icmp ne ptr %handler, null
  br i1 %handled, label %transfer, label %uncaught
transfer:
  store ptr %exception, ptr @__scalanative_current_exception
  %previous_field = getelementptr %scalanative.exception_handler, ptr %handler, i32 0, i32 0
  %previous = load ptr, ptr %previous_field
  %shadow_field = getelementptr %scalanative.exception_handler, ptr %handler, i32 0, i32 1
  %shadow = load ptr, ptr %shadow_field
  %zone_field = getelementptr %scalanative.exception_handler, ptr %handler, i32 0, i32 2
  %zone = load ptr, ptr %zone_field
  %jump_field = getelementptr %scalanative.exception_handler, ptr %handler, i32 0, i32 3
  %jump = load ptr, ptr %jump_field
  %source_field = getelementptr %scalanative.exception_handler, ptr %handler, i32 0, i32 4
  %source = load ptr, ptr %source_field
  store ptr %previous, ptr @__scalanative_exception_handler
  store ptr %shadow, ptr @__scalanative_shadow_stack
  store ptr %source, ptr @__scalanative_source_stack
  call void @__scalanative_zone_unwind_to(ptr %zone)
  call void @longjmp(ptr %jump, i32 1)
  unreachable
uncaught:
  store ptr %exception, ptr @__scalanative_current_exception
  call void @__scalanative_report_uncaught_exception(ptr %exception)
  call void @__scalanative_runtime_shutdown()
  %flushed = call i32 @fflush(ptr null)
  call void @abort()
  unreachable
}
