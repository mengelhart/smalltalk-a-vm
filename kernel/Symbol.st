"Symbol kernel methods.
 Symbol is an immutable, interned String subclass.
 Primitive methods installed by bootstrap: asString (prim 92).
 Do NOT redefine asString here — bootstrap installs it as prim 92.
 Symbol class >> intern: (prim 93) installed on metaclass."

!Symbol methodsFor: 'converting'!
asSymbol
	^ self! !

!Symbol methodsFor: 'copying'!
, aString
	"Concatenation yields a String, not a Symbol."
	^ self asString , aString! !

!Symbol methodsFor: 'printing'!
printString
	^ '#', self asString! !
