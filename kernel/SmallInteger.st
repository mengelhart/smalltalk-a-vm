"Kernel methods for SmallInteger.
 Do NOT duplicate primitive methods installed by bootstrap.c step 7:
   +, -, *, <, >, =, <=, >=, ~=, \\, //, quo:,
   bitAnd:, bitOr:, bitXor:, bitShift:, printString, basicNew, basicNew:
 EXCEPTION: / is overridden here to signal ZeroDivide (#339).
 SKIPPED: @ (no Point class)."

!SmallInteger methodsFor: 'arithmetic'!
/ aNumber
	"Exact integer division. Signals ZeroDivide if aNumber is zero.
	 For non-exact results, returns the truncated quotient (no Fraction class yet)."
	| result |
	aNumber = 0
		ifTrue: [ZeroDivide new signal]
		ifFalse: [result := self quo: aNumber].
	^ result! !

!SmallInteger methodsFor: 'testing'!
isInteger
	^ true!
asInteger
	^ self! !

!SmallInteger methodsFor: 'printing'!
printString: base
	"Return string representation in given radix."
	| n result |
	self = 0
		ifTrue: [result := '0']
		ifFalse: [
			self < 0
				ifTrue: [result := '-', (self negated printString: base)]
				ifFalse: [
					n := self. result := ''.
					[n > 0] whileTrue: [
						| digit |
						digit := n \\ base.
						result := ('0123456789ABCDEF' at: digit + 1) asString, result.
						n := n // base]]].
	^ result!
printStringHex
	^ self printString: 16!
printStringOctal
	^ self printString: 8!
printStringBinary
	^ self printString: 2! !

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
