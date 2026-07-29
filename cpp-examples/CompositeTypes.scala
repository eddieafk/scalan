package demo.compositetypes

trait RequestId
trait Username
trait Audited

class RequestIdValue extends RequestId
class UsernameValue extends Username
class AuditedRequest extends RequestId with Audited
class AuditedUsername extends Username with Audited

type LookupKey = RequestId | Username
type AuditedKey = LookupKey & Audited
type EitherOf[A, B] = A | B
type BothOf[A, B] = A & B

object CompositeTypes {
  def route(key: LookupKey): String = "routed"
  def routeGeneric(key: EitherOf[RequestId, Username]): String =
    "routed-generic"
  def record(key: AuditedKey): String = "recorded"
  def recordGeneric(key: BothOf[LookupKey, Audited]): String =
    "recorded-generic"

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
  }
}
