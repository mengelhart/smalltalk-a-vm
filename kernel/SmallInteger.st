"Kernel methods for SmallInteger.
 Do NOT duplicate primitive methods installed by bootstrap.c step 7:
   +, -, *, <, >, =, basicNew, basicNew:
 SKIPPED for Phase 1: //, \\ (no primitives), printString (needs String),
   @ (no Point class)."

!SmallInteger methodsFor: 'mathematical functions'!

factorial
    | result |
    self = 0
        ifTrue: [result := 1]
        ifFalse: [result := self * (self - 1) factorial].
    ^ result
! !

