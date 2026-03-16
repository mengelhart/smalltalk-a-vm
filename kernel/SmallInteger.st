"Kernel methods for SmallInteger.
 Do NOT duplicate primitive methods installed by bootstrap.c step 7:
   +, -, *, <, >, =, <=, >=, ~=, /, \\, //, quo:,
   bitAnd:, bitOr:, bitXor:, bitShift:, printString, basicNew, basicNew:
 SKIPPED: @ (no Point class)."

!SmallInteger methodsFor: 'mathematical functions'!

factorial
    | result |
    self = 0
        ifTrue: [result := 1]
        ifFalse: [result := self * (self - 1) factorial].
    ^ result!

even
    ^ (self \\ 2) = 0!

odd
    ^ self even not!

gcd: anInteger
    "Euclidean algorithm. Not tail-recursive due to temp pattern."
    | result |
    anInteger = 0
        ifTrue: [result := self abs]
        ifFalse: [result := anInteger gcd: (self \\ anInteger)].
    ^ result!

lcm: anInteger
    "Least common multiple."
    | result |
    self = 0
        ifTrue: [result := 0]
        ifFalse: [anInteger = 0
            ifTrue: [result := 0]
            ifFalse: [result := (self * anInteger) abs // (self gcd: anInteger)]].
    ^ result! !
