SECTIONS {
   .init.rodata.schedules : ALIGN(8) {
        PROVIDE_HIDDEN(__schedules_start = .);
         *(.init.rodata.schedules)
        PROVIDE_HIDDEN(__schedules_end = .);
   } 
} INSERT AFTER .init.rodata