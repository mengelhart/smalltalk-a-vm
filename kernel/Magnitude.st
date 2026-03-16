"Kernel methods for Magnitude.
 Magnitude defines comparison protocol. Subclasses must implement <."

!Magnitude methodsFor: 'comparing'!
>= aValue
	^ (self < aValue) not!
<= aValue
	^ (aValue < self) not!
max: aValue
	| result |
	self >= aValue
		ifTrue: [result := self]
		ifFalse: [result := aValue].
	^ result!
min: aValue
	| result |
	self <= aValue
		ifTrue: [result := self]
		ifFalse: [result := aValue].
	^ result!
between: aMin and: aMax
	^ self >= aMin and: [self <= aMax]! !
