#ifndef OURS_TASK_AUTO_MIGRATE_DISABLER_HPP
#define OURS_TASK_AUTO_MIGRATE_DISABLER_HPP

namespace ours::task {
    class AutoMigrateDisabler {
        using Self = AutoMigrateDisabler;
      public:
        AutoMigrateDisabler() {}
        ~AutoMigrateDisabler() {}
      private:
        bool m_disabled;
    }; // class AutoMigrateDisabler
     
} // namespace ours::task

#endif // OURS_TASK_AUTO_MIGRATE_DISABLER_HPP
