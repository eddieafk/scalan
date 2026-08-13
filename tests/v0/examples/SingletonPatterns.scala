package demo.examples

trait Signal

object Ready extends Signal

object Waiting extends Signal

object State {
  object Ready extends Signal

  object Waiting extends Signal

  def ready: Signal = Ready

  def waiting: Signal = Waiting
}

class Other extends Signal

object SingletonPatterns {
  def describe(value: Signal): String = value match {
    case Ready => "ready"
    case Waiting => "waiting"
    case _ => "other"
  }

  def known(value: Signal): String = value match {
    case Ready | Waiting => "known"
    case _ => "other"
  }

  def describeQualified(value: Signal): String = value match {
    case State.Ready => "state-ready"
    case State.Waiting => "state-waiting"
    case _ => "other"
  }

  def main = {
    println(describe(Ready))
    println(describe(Waiting))
    println(describe(new Other()))
    println(known(Ready))
    println(known(Waiting))
    println(known(new Other()))
    println(describeQualified(State.ready))
    println(describeQualified(State.waiting))
    println(describeQualified(new Other()))
  }
}
