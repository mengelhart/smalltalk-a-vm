"Kernel methods for Number.
 Number adds arithmetic protocol on top of Magnitude.
 SKIPPED: reciprocal (no Fraction class yet)."

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

!Number methodsFor: 'enumerating'!
to: stop do: aBlock
	| i |
	i := self.
	[i <= stop] whileTrue: [
		aBlock value: i.
		i := i + 1]!
to: stop by: step do: aBlock
	| i |
	i := self.
	step > 0
		ifTrue: [[i <= stop] whileTrue: [
			aBlock value: i.
			i := i + step]]
		ifFalse: [[i >= stop] whileTrue: [
			aBlock value: i.
			i := i + step]]!
timesRepeat: aBlock
	| i |
	i := 1.
	[i <= self] whileTrue: [
		aBlock value.
		i := i + 1]! !
