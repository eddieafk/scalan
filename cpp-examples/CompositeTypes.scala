package demo.compositetypes

trait LookupView {
  def display(): String
}
trait RequestId extends LookupView {
  def requestPart(): String
}
trait Username extends LookupView {
  def usernamePart(): String
}
trait Audited {
  def auditLabel(): String
}
transparent trait RoutingMarker

class RequestIdValue extends RequestId with RoutingMarker {
  def display(): String = "request"
  def requestPart(): String = "request-part"
}
class UsernameValue extends Username with RoutingMarker {
  def display(): String = "username"
  def usernamePart(): String = "username-part"
}
class AuditedRequest extends RequestId with Audited with RoutingMarker {
  def display(): String = "audited-request"
  def requestPart(): String = "audited-request-part"
  def auditLabel(): String = "request-audit"
}
class AuditedUsername extends Username with Audited with RoutingMarker {
  def display(): String = "audited-username"
  def usernamePart(): String = "audited-username-part"
  def auditLabel(): String = "username-audit"
}

type LookupKey = RequestId | Username
type AuditedKey = LookupKey & Audited
type EitherOf[A, B] = A | B
type BothOf[A, B] = A & B

object CompositeTypes {
  def route(key: LookupKey): String = key.display()
  def routeGeneric(key: EitherOf[RequestId, Username]): String =
    key.display()
  def record(key: AuditedKey): String = key.auditLabel()
  def recordGeneric(key: BothOf[LookupKey, Audited]): String =
    key.auditLabel()
  def firstOf[A](left: A, right: A): A = left
  def inferredLookup(useRequest: Boolean) =
    if (useRequest) new RequestIdValue else new UsernameValue
  def inferredAudited(useRequest: Boolean) =
    if (useRequest) new AuditedRequest else new AuditedUsername
  def inferredGeneric(): String =
    firstOf(new RequestIdValue, new UsernameValue).display()
  def retainedGenericUnion(): String =
    firstOf("retained-generic-union", 23).asInstanceOf[String]

  def main(args: Array[String]): Unit = {
    val request: LookupKey = new RequestIdValue
    val username: EitherOf[RequestId, Username] = new UsernameValue
    val auditedRequest: AuditedKey = new AuditedRequest
    val auditedUsername: BothOf[LookupKey, Audited] = new AuditedUsername

    println(route(request))
    println(route(username))
    println(routeGeneric(request))
    println(record(auditedRequest))
    println(record(auditedUsername))
    println(recordGeneric(auditedRequest))
    println(inferredLookup(true).display())
    println(inferredLookup(false).display())
    println(inferredAudited(false).auditLabel())
    println(inferredGeneric())
    println(retainedGenericUnion())
  }
}
