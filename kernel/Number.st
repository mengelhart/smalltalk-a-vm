"Kernel methods for Number.
 Number adds arithmetic protocol on top of Magnitude.
 SKIPPED for Phase 1: to:do:, to:by:do:, timesRepeat: (need mutable closures).
 SKIPPED: reciprocal (no / primitive for SmallInteger yet)."

!Number methodsFor: 'testing'!
isZero
	^ self = 0!
positive
	^ self >= 0!
negative
	^ self < 0! !

!Number methodsFor: 'arithmetic'!
negated
	^ 0 - self!
abs
	| result |
	self < 0
		ifTrue: [result := 0 - self]
		ifFalse: [result := self].
	^ result!
sign
	| result |
	self > 0
		ifTrue: [result := 1]
		ifFalse: [self < 0
			ifTrue: [result := -1]
			ifFalse: [result := 0]].
	^ result! !
