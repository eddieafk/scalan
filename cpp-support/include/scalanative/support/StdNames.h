#pragma once

#include <string_view>

namespace scalanative::support {

struct StdNames final {
  inline static constexpr std::string_view ScalaPackage = "scala";
  inline static constexpr std::string_view JavaLangObject = "java.lang.Object";
  inline static constexpr std::string_view JavaLangThrowable = "java.lang.Throwable";
  inline static constexpr std::string_view JavaLangError = "java.lang.Error";
  inline static constexpr std::string_view JavaLangAssertionError =
      "java.lang.AssertionError";
  inline static constexpr std::string_view ScalaNotImplementedError =
      "scala.NotImplementedError";
  inline static constexpr std::string_view JavaLangException = "java.lang.Exception";
  inline static constexpr std::string_view JavaLangRuntimeException =
      "java.lang.RuntimeException";
  inline static constexpr std::string_view JavaLangArithmeticException =
      "java.lang.ArithmeticException";
  inline static constexpr std::string_view JavaLangStackTraceElement =
      "java.lang.StackTraceElement";
  inline static constexpr std::string_view RuntimeSuppressedExceptionNode =
      "scala.scalanative.runtime.SuppressedExceptionNode";
  inline static constexpr std::string_view JavaLangIllegalArgumentException =
      "java.lang.IllegalArgumentException";
  inline static constexpr std::string_view JavaLangIllegalStateException =
      "java.lang.IllegalStateException";
  inline static constexpr std::string_view JavaLangNullPointerException =
      "java.lang.NullPointerException";
  inline static constexpr std::string_view JavaLangClassCastException =
      "java.lang.ClassCastException";
  inline static constexpr std::string_view JavaLangArrayStoreException =
      "java.lang.ArrayStoreException";
  inline static constexpr std::string_view JavaLangIndexOutOfBoundsException =
      "java.lang.IndexOutOfBoundsException";
  inline static constexpr std::string_view JavaLangArrayIndexOutOfBoundsException =
      "java.lang.ArrayIndexOutOfBoundsException";
  inline static constexpr std::string_view JavaLangNegativeArraySizeException =
      "java.lang.NegativeArraySizeException";
  inline static constexpr std::string_view JavaNioByteBuffer = "java.nio.ByteBuffer";
  inline static constexpr std::string_view ThrowableMessage = "message";
  inline static constexpr std::string_view ThrowableCause = "cause";
  inline static constexpr std::string_view ThrowableTrace = "stackTrace";
  inline static constexpr std::string_view ThrowableSuppressedHead = "suppressedHead";
  inline static constexpr std::string_view ThrowableSuppressedCount = "suppressedCount";
  inline static constexpr std::string_view SuppressedExceptionValue = "exception";
  inline static constexpr std::string_view SuppressedExceptionNext = "next";
  inline static constexpr std::string_view GetMessage = "getMessage";
  inline static constexpr std::string_view GetCause = "getCause";
  inline static constexpr std::string_view InitCause = "initCause";
  inline static constexpr std::string_view FillInStackTrace = "fillInStackTrace";
  inline static constexpr std::string_view GetStackTrace = "getStackTrace";
  inline static constexpr std::string_view SetStackTrace = "setStackTrace";
  inline static constexpr std::string_view AddSuppressed = "addSuppressed";
  inline static constexpr std::string_view GetSuppressed = "getSuppressed";
  inline static constexpr std::string_view PrintStackTrace = "printStackTrace";
  inline static constexpr std::string_view StackTraceFunctionName = "functionName";
  inline static constexpr std::string_view StackTraceFileName = "fileName";
  inline static constexpr std::string_view StackTraceLineNumber = "lineNumber";
  inline static constexpr std::string_view StackTraceColumnNumber = "columnNumber";
  inline static constexpr std::string_view This = "this";
  inline static constexpr std::string_view Super = "super";
  inline static constexpr std::string_view Constructor = "$init";
  inline static constexpr std::string_view IsInstanceOf = "isInstanceOf";
  inline static constexpr std::string_view AsInstanceOf = "asInstanceOf";
  inline static constexpr std::string_view Assert = "assert";
  inline static constexpr std::string_view Assume = "assume";
  inline static constexpr std::string_view NotImplemented = "???";
  inline static constexpr std::string_view Require = "require";
  inline static constexpr std::string_view Println = "println";
  inline static constexpr std::string_view GcCollect = "gcCollect";
  inline static constexpr std::string_view GcLiveObjectCount = "gcLiveObjectCount";
  inline static constexpr std::string_view GcCollectionCount = "gcCollectionCount";
  inline static constexpr std::string_view GcSetCollectionThreshold =
      "gcSetCollectionThreshold";
  inline static constexpr std::string_view SizeOf = "sizeof";
  inline static constexpr std::string_view StringLength = "length";
  inline static constexpr std::string_view ToString = "toString";
  inline static constexpr std::string_view ToByte = "toByte";
  inline static constexpr std::string_view ToShort = "toShort";
  inline static constexpr std::string_view ToInt = "toInt";
  inline static constexpr std::string_view Equals = "equals";
  inline static constexpr std::string_view HashCode = "hashCode";
  inline static constexpr std::string_view Zone = "Zone";
  inline static constexpr std::string_view ZoneScoped = "scoped";
  inline static constexpr std::string_view ZoneAllocBytes = "allocBytes";
  inline static constexpr std::string_view NativeBytes = "NativeBytes";
  inline static constexpr std::string_view NativeBytesGetShortBe = "getShortBE";
  inline static constexpr std::string_view NativeBytesGetShortLe = "getShortLE";
  inline static constexpr std::string_view NativeBytesPutShortBe = "putShortBE";
  inline static constexpr std::string_view NativeBytesPutShortLe = "putShortLE";
  inline static constexpr std::string_view ByteBuffer = "ByteBuffer";
  inline static constexpr std::string_view ByteBufferWrap = "wrap";
  inline static constexpr std::string_view ByteBufferCapacity = "capacity";
  inline static constexpr std::string_view ByteBufferPosition = "position";
  inline static constexpr std::string_view ByteBufferLimit = "limit";
  inline static constexpr std::string_view ByteBufferRemaining = "remaining";
  inline static constexpr std::string_view ByteBufferHasRemaining = "hasRemaining";
  inline static constexpr std::string_view ByteBufferClear = "clear";
  inline static constexpr std::string_view ByteBufferFlip = "flip";
  inline static constexpr std::string_view ByteBufferRewind = "rewind";
  inline static constexpr std::string_view ArrayEmpty = "empty";
  inline static constexpr std::string_view ArrayFill = "fill";
  inline static constexpr std::string_view ArrayRange = "range";
  inline static constexpr std::string_view ArrayConcat = "concat";
  inline static constexpr std::string_view ArrayOfDim = "ofDim";
  inline static constexpr std::string_view ArrayClone = "clone";
  inline static constexpr std::string_view ArrayCopy = "copy";
  inline static constexpr std::string_view RuntimeMain =
      "scala.scalanative.runtime.main";
  inline static constexpr std::string_view RuntimePrintln =
      "scala.scalanative.runtime.println";
  inline static constexpr std::string_view RuntimeAssert =
      "scala.scalanative.runtime.assert";
  inline static constexpr std::string_view RuntimeAssume =
      "scala.scalanative.runtime.assume";
  inline static constexpr std::string_view RuntimeRequire =
      "scala.scalanative.runtime.require";
  inline static constexpr std::string_view RuntimeGcCollect =
      "scala.scalanative.runtime.gcCollect";
  inline static constexpr std::string_view RuntimeGcLiveObjectCount =
      "scala.scalanative.runtime.gcLiveObjectCount";
  inline static constexpr std::string_view RuntimeGcCollectionCount =
      "scala.scalanative.runtime.gcCollectionCount";
  inline static constexpr std::string_view RuntimeGcSetCollectionThreshold =
      "scala.scalanative.runtime.gcSetCollectionThreshold";
  inline static constexpr std::string_view RuntimeZoneAllocBytes =
      "scala.scalanative.runtime.zoneAllocBytes";
  inline static constexpr std::string_view RuntimeNativeBytesGetShortBe =
      "scala.scalanative.runtime.nativeBytesGetShortBE";
  inline static constexpr std::string_view RuntimeNativeBytesGetShortLe =
      "scala.scalanative.runtime.nativeBytesGetShortLE";
  inline static constexpr std::string_view RuntimeNativeBytesPutShortBe =
      "scala.scalanative.runtime.nativeBytesPutShortBE";
  inline static constexpr std::string_view RuntimeNativeBytesPutShortLe =
      "scala.scalanative.runtime.nativeBytesPutShortLE";
  inline static constexpr std::string_view RuntimeByteBufferWrap =
      "scala.scalanative.runtime.byteBufferWrap";
  inline static constexpr std::string_view RuntimeByteBufferCapacity =
      "scala.scalanative.runtime.byteBufferCapacity";
  inline static constexpr std::string_view RuntimeByteBufferPosition =
      "scala.scalanative.runtime.byteBufferPosition";
  inline static constexpr std::string_view RuntimeByteBufferSetPosition =
      "scala.scalanative.runtime.byteBufferSetPosition";
  inline static constexpr std::string_view RuntimeByteBufferLimit =
      "scala.scalanative.runtime.byteBufferLimit";
  inline static constexpr std::string_view RuntimeByteBufferSetLimit =
      "scala.scalanative.runtime.byteBufferSetLimit";
  inline static constexpr std::string_view RuntimeByteBufferRemaining =
      "scala.scalanative.runtime.byteBufferRemaining";
  inline static constexpr std::string_view RuntimeByteBufferHasRemaining =
      "scala.scalanative.runtime.byteBufferHasRemaining";
  inline static constexpr std::string_view RuntimeByteBufferClear =
      "scala.scalanative.runtime.byteBufferClear";
  inline static constexpr std::string_view RuntimeByteBufferFlip =
      "scala.scalanative.runtime.byteBufferFlip";
  inline static constexpr std::string_view RuntimeByteBufferRewind =
      "scala.scalanative.runtime.byteBufferRewind";
  inline static constexpr std::string_view RuntimeStringLength =
      "scala.scalanative.runtime.stringLength";
  inline static constexpr std::string_view RuntimeStringToString =
      "scala.scalanative.runtime.stringToString";
  inline static constexpr std::string_view RuntimeStringEquals =
      "scala.scalanative.runtime.stringEquals";
  inline static constexpr std::string_view RuntimeArrayLength =
      "scala.scalanative.runtime.arrayLength";
  inline static constexpr std::string_view RuntimeArrayAlloc =
      "scala.scalanative.runtime.arrayAlloc";
  inline static constexpr std::string_view RuntimeArrayApply =
      "scala.scalanative.runtime.arrayApply";
  inline static constexpr std::string_view RuntimeArrayUpdate =
      "scala.scalanative.runtime.arrayUpdate";
  inline static constexpr std::string_view RuntimeArrayClone =
      "scala.scalanative.runtime.arrayClone";
  inline static constexpr std::string_view RuntimeIntArrayLength =
      "scala.scalanative.runtime.intArrayLength";
  inline static constexpr std::string_view RuntimeIntArrayAlloc =
      "scala.scalanative.runtime.intArrayAlloc";
  inline static constexpr std::string_view RuntimeIntArrayApply =
      "scala.scalanative.runtime.intArrayApply";
  inline static constexpr std::string_view RuntimeIntArrayUpdate =
      "scala.scalanative.runtime.intArrayUpdate";
  inline static constexpr std::string_view RuntimeIntArrayClone =
      "scala.scalanative.runtime.intArrayClone";
  inline static constexpr std::string_view RuntimeByteArrayLength =
      "scala.scalanative.runtime.byteArrayLength";
  inline static constexpr std::string_view RuntimeByteArrayAlloc =
      "scala.scalanative.runtime.byteArrayAlloc";
  inline static constexpr std::string_view RuntimeByteArrayApply =
      "scala.scalanative.runtime.byteArrayApply";
  inline static constexpr std::string_view RuntimeByteArrayUpdate =
      "scala.scalanative.runtime.byteArrayUpdate";
  inline static constexpr std::string_view RuntimeByteArrayClone =
      "scala.scalanative.runtime.byteArrayClone";
  inline static constexpr std::string_view RuntimeShortArrayLength =
      "scala.scalanative.runtime.shortArrayLength";
  inline static constexpr std::string_view RuntimeShortArrayAlloc =
      "scala.scalanative.runtime.shortArrayAlloc";
  inline static constexpr std::string_view RuntimeShortArrayApply =
      "scala.scalanative.runtime.shortArrayApply";
  inline static constexpr std::string_view RuntimeShortArrayUpdate =
      "scala.scalanative.runtime.shortArrayUpdate";
  inline static constexpr std::string_view RuntimeShortArrayClone =
      "scala.scalanative.runtime.shortArrayClone";
  inline static constexpr std::string_view RuntimeBooleanArrayLength =
      "scala.scalanative.runtime.booleanArrayLength";
  inline static constexpr std::string_view RuntimeBooleanArrayAlloc =
      "scala.scalanative.runtime.booleanArrayAlloc";
  inline static constexpr std::string_view RuntimeBooleanArrayApply =
      "scala.scalanative.runtime.booleanArrayApply";
  inline static constexpr std::string_view RuntimeBooleanArrayUpdate =
      "scala.scalanative.runtime.booleanArrayUpdate";
  inline static constexpr std::string_view RuntimeBooleanArrayClone =
      "scala.scalanative.runtime.booleanArrayClone";
  inline static constexpr std::string_view RuntimeLongArrayLength =
      "scala.scalanative.runtime.longArrayLength";
  inline static constexpr std::string_view RuntimeLongArrayAlloc =
      "scala.scalanative.runtime.longArrayAlloc";
  inline static constexpr std::string_view RuntimeLongArrayApply =
      "scala.scalanative.runtime.longArrayApply";
  inline static constexpr std::string_view RuntimeLongArrayUpdate =
      "scala.scalanative.runtime.longArrayUpdate";
  inline static constexpr std::string_view RuntimeLongArrayClone =
      "scala.scalanative.runtime.longArrayClone";
  inline static constexpr std::string_view RuntimeDoubleArrayLength =
      "scala.scalanative.runtime.doubleArrayLength";
  inline static constexpr std::string_view RuntimeDoubleArrayAlloc =
      "scala.scalanative.runtime.doubleArrayAlloc";
  inline static constexpr std::string_view RuntimeDoubleArrayApply =
      "scala.scalanative.runtime.doubleArrayApply";
  inline static constexpr std::string_view RuntimeDoubleArrayUpdate =
      "scala.scalanative.runtime.doubleArrayUpdate";
  inline static constexpr std::string_view RuntimeDoubleArrayClone =
      "scala.scalanative.runtime.doubleArrayClone";
  inline static constexpr std::string_view RuntimeFloatArrayLength =
      "scala.scalanative.runtime.floatArrayLength";
  inline static constexpr std::string_view RuntimeFloatArrayAlloc =
      "scala.scalanative.runtime.floatArrayAlloc";
  inline static constexpr std::string_view RuntimeFloatArrayApply =
      "scala.scalanative.runtime.floatArrayApply";
  inline static constexpr std::string_view RuntimeFloatArrayUpdate =
      "scala.scalanative.runtime.floatArrayUpdate";
  inline static constexpr std::string_view RuntimeFloatArrayClone =
      "scala.scalanative.runtime.floatArrayClone";
  inline static constexpr std::string_view RuntimeCharArrayLength =
      "scala.scalanative.runtime.charArrayLength";
  inline static constexpr std::string_view RuntimeCharArrayAlloc =
      "scala.scalanative.runtime.charArrayAlloc";
  inline static constexpr std::string_view RuntimeCharArrayApply =
      "scala.scalanative.runtime.charArrayApply";
  inline static constexpr std::string_view RuntimeCharArrayUpdate =
      "scala.scalanative.runtime.charArrayUpdate";
  inline static constexpr std::string_view RuntimeCharArrayClone =
      "scala.scalanative.runtime.charArrayClone";
  inline static constexpr std::string_view RuntimeReferenceArrayLength =
      "scala.scalanative.runtime.referenceArrayLength";
  inline static constexpr std::string_view RuntimeReferenceArrayAlloc =
      "scala.scalanative.runtime.referenceArrayAlloc";
  inline static constexpr std::string_view RuntimeReferenceArrayApply =
      "scala.scalanative.runtime.referenceArrayApply";
  inline static constexpr std::string_view RuntimeReferenceArrayUpdate =
      "scala.scalanative.runtime.referenceArrayUpdate";
  inline static constexpr std::string_view RuntimeReferenceArrayClone =
      "scala.scalanative.runtime.referenceArrayClone";
  inline static constexpr std::string_view RuntimeArrayOfDim =
      "scala.scalanative.runtime.arrayOfDim";
  inline static constexpr std::string_view RuntimeArrayFill =
      "scala.scalanative.runtime.arrayFill";
  inline static constexpr std::string_view RuntimeArrayRange =
      "scala.scalanative.runtime.arrayRange";
  inline static constexpr std::string_view RuntimeArrayConcat =
      "scala.scalanative.runtime.arrayConcat";
  inline static constexpr std::string_view RuntimeArrayCopy =
      "scala.scalanative.runtime.arrayCopy";
  inline static constexpr std::string_view RuntimeReferenceArrayCopy =
      "scala.scalanative.runtime.referenceArrayCopy";
  inline static constexpr std::string_view RuntimeBooleanToString =
      "scala.scalanative.runtime.booleanToString";
  inline static constexpr std::string_view RuntimeByteToString =
      "scala.scalanative.runtime.byteToString";
  inline static constexpr std::string_view RuntimeShortToString =
      "scala.scalanative.runtime.shortToString";
  inline static constexpr std::string_view RuntimeIntToString =
      "scala.scalanative.runtime.intToString";
  inline static constexpr std::string_view RuntimeLongToString =
      "scala.scalanative.runtime.longToString";
  inline static constexpr std::string_view RuntimeFloatToString =
      "scala.scalanative.runtime.floatToString";
  inline static constexpr std::string_view RuntimeDoubleToString =
      "scala.scalanative.runtime.doubleToString";
  inline static constexpr std::string_view RuntimeCharToString =
      "scala.scalanative.runtime.charToString";
  inline static constexpr std::string_view RuntimeAnyToString =
      "scala.scalanative.runtime.anyToString";
  inline static constexpr std::string_view RuntimeAnyReceiverToString =
      "scala.scalanative.runtime.anyReceiverToString";
  inline static constexpr std::string_view RuntimeThrowableToString =
      "scala.scalanative.runtime.throwableToString";
  inline static constexpr std::string_view RuntimePrintStackTrace =
      "scala.scalanative.runtime.printStackTrace";
  inline static constexpr std::string_view RuntimeFillInStackTrace =
      "scala.scalanative.runtime.fillInStackTrace";
  inline static constexpr std::string_view RuntimeGetStackTrace =
      "scala.scalanative.runtime.getStackTrace";
  inline static constexpr std::string_view RuntimeSetStackTrace =
      "scala.scalanative.runtime.setStackTrace";
  inline static constexpr std::string_view RuntimeAddSuppressed =
      "scala.scalanative.runtime.addSuppressed";
  inline static constexpr std::string_view RuntimeGetSuppressed =
      "scala.scalanative.runtime.getSuppressed";
  inline static constexpr std::string_view RuntimeStackTraceElementToString =
      "scala.scalanative.runtime.stackTraceElementToString";
  inline static constexpr std::string_view RuntimeAnyEquals =
      "scala.scalanative.runtime.anyEquals";
  inline static constexpr std::string_view RuntimeAnyReceiverEquals =
      "scala.scalanative.runtime.anyReceiverEquals";
  inline static constexpr std::string_view RuntimeIntToByte =
      "scala.scalanative.runtime.intToByte";
  inline static constexpr std::string_view RuntimeIntToShort =
      "scala.scalanative.runtime.intToShort";
  inline static constexpr std::string_view RuntimeShortToByte =
      "scala.scalanative.runtime.shortToByte";
  inline static constexpr std::string_view RuntimeByteToShort =
      "scala.scalanative.runtime.byteToShort";
  inline static constexpr std::string_view RuntimeByteToInt =
      "scala.scalanative.runtime.byteToInt";
  inline static constexpr std::string_view RuntimeShortToInt =
      "scala.scalanative.runtime.shortToInt";
  inline static constexpr std::string_view RuntimeByteHashCode =
      "scala.scalanative.runtime.byteHashCode";
  inline static constexpr std::string_view RuntimeShortHashCode =
      "scala.scalanative.runtime.shortHashCode";
  inline static constexpr std::string_view RuntimeBooleanHashCode =
      "scala.scalanative.runtime.booleanHashCode";
  inline static constexpr std::string_view RuntimeLongHashCode =
      "scala.scalanative.runtime.longHashCode";
  inline static constexpr std::string_view RuntimeFloatHashCode =
      "scala.scalanative.runtime.floatHashCode";
  inline static constexpr std::string_view RuntimeDoubleHashCode =
      "scala.scalanative.runtime.doubleHashCode";
  inline static constexpr std::string_view RuntimeCharHashCode =
      "scala.scalanative.runtime.charHashCode";
  inline static constexpr std::string_view RuntimeStringHashCode =
      "scala.scalanative.runtime.stringHashCode";
  inline static constexpr std::string_view RuntimeSymbolHashCode =
      "scala.scalanative.runtime.symbolHashCode";
  inline static constexpr std::string_view RuntimeAnyHashCode =
      "scala.scalanative.runtime.anyHashCode";
  inline static constexpr std::string_view RuntimeAnyReceiverHashCode =
      "scala.scalanative.runtime.anyReceiverHashCode";
  inline static constexpr std::string_view RuntimeFormat =
      "scala.scalanative.runtime.format";
  inline static constexpr std::string_view RuntimeFormatBoolean =
      "scala.scalanative.runtime.formatBoolean";
};

} // namespace scalanative::support
