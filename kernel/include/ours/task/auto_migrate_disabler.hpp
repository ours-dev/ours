#ifndef OURS_TASK_AUTO_MIGRATE_DISABLER_HPP
#define OURS_TASK_AUTO_MIGRATE_DISABLER_HPP

namespace ours::task {
class AutoMigrateDisabler {
    using Self = AutoMigrateDisabler;

  public:
    AutoMigrateDisabler() {}
    ~AutoMigrateDisabler() {}

  private:
    /// Whether this scope still holds migration off, as AutoPreemptDisabler
    /// keeps for preemption: a scope object's one fact, so it has no word to
    /// share. Nothing sets it yet.
    bool m_disabled;
}; // class AutoMigrateDisabler

} // namespace ours::task

#endif // OURS_TASK_AUTO_MIGRATE_DISABLER_HPP
